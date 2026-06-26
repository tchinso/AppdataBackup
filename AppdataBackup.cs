using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("Appdata Backup")]
[assembly: AssemblyDescription("Bandizip based backup and restore utility")]
[assembly: AssemblyCompany("Personal")]
[assembly: AssemblyProduct("Appdata Backup")]
[assembly: AssemblyVersion("1.1.0.0")]
[assembly: AssemblyFileVersion("1.1.0.0")]

namespace AppdataBackup
{
    internal enum Operation
    {
        BackupAppdata,
        BackupStash,
        BackupWiki,
        BackupAll,
        RestoreAppdata,
        RestoreStash,
        RestoreWiki,
        RestoreAll
    }

    internal sealed class BackupConfig
    {
        public int AppdataLevel = 4;
        public int StashLevel = 1;
        public int WikiLevel = 9;
        public double LocalThresholdMiB = 25.6;
        public string LocalExcludedFolders = "ConnectedDevicesPlatform;NVIDIA";
        public string BandizipPath = "";

        public static BackupConfig Load(string path)
        {
            BackupConfig config = new BackupConfig();
            if (!File.Exists(path)) return config;

            Dictionary<string, Dictionary<string, string>> ini = ReadIni(path);
            config.AppdataLevel = ValidLevel(GetInt(ini, "Compression", "Appdata", 4), 4);
            config.StashLevel = ValidLevel(GetInt(ini, "Compression", "Stash", 1), 1);
            config.WikiLevel = ValidLevel(GetInt(ini, "Compression", "PersonalWiki", 9), 9);
            config.LocalThresholdMiB = GetDouble(ini, "Appdata", "LocalThresholdMiB", 25.6, 0.1, 1048576.0);
            config.LocalExcludedFolders = GetString(ini, "Appdata", "LocalExcludedFolders",
                "ConnectedDevicesPlatform;NVIDIA");
            config.BandizipPath = GetString(ini, "Bandizip", "Path", "");
            return config;
        }

        public void Save(string path)
        {
            string[] lines = {
                "[Compression]",
                "Appdata=" + AppdataLevel.ToString(),
                "Stash=" + StashLevel.ToString(),
                "PersonalWiki=" + WikiLevel.ToString(),
                "",
                "[Appdata]",
                "LocalThresholdMiB=" + LocalThresholdMiB.ToString("0.0"),
                "LocalExcludedFolders=" + LocalExcludedFolders,
                "",
                "[Bandizip]",
                "Path=" + BandizipPath
            };
            File.WriteAllLines(path, lines, Encoding.UTF8);
        }

        private static int ValidLevel(int value, int fallback)
        {
            return value == 1 || value == 4 || value == 9 ? value : fallback;
        }

        private static Dictionary<string, Dictionary<string, string>> ReadIni(string path)
        {
            Dictionary<string, Dictionary<string, string>> ini =
                new Dictionary<string, Dictionary<string, string>>(StringComparer.OrdinalIgnoreCase);
            string section = "";
            ini[section] = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

            foreach (string rawLine in File.ReadAllLines(path, Encoding.UTF8))
            {
                string line = rawLine.Trim();
                if (line.Length == 0 || line.StartsWith(";") || line.StartsWith("#")) continue;
                if (line.StartsWith("[") && line.EndsWith("]"))
                {
                    section = line.Substring(1, line.Length - 2).Trim();
                    if (!ini.ContainsKey(section))
                        ini[section] = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                    continue;
                }
                int equals = line.IndexOf('=');
                if (equals < 0) continue;
                ini[section][line.Substring(0, equals).Trim()] = line.Substring(equals + 1).Trim();
            }
            return ini;
        }

        private static string GetString(Dictionary<string, Dictionary<string, string>> ini,
            string section, string key, string fallback)
        {
            Dictionary<string, string> values;
            string value;
            return ini.TryGetValue(section, out values) && values.TryGetValue(key, out value)
                ? value
                : fallback;
        }

        private static int GetInt(Dictionary<string, Dictionary<string, string>> ini,
            string section, string key, int fallback)
        {
            int value;
            return int.TryParse(GetString(ini, section, key, ""), out value) ? value : fallback;
        }

        private static double GetDouble(Dictionary<string, Dictionary<string, string>> ini,
            string section, string key, double fallback, double min, double max)
        {
            double value;
            if (!double.TryParse(GetString(ini, section, key, ""), out value)) return fallback;
            return value >= min && value <= max ? value : fallback;
        }
    }

    internal sealed class MainForm : Form
    {
        private readonly string exeDir;
        private readonly string cfgPath;
        private BackupConfig config;
        private readonly Button[] buttons = new Button[9];
        private Label statusLabel;
        private TextBox logBox;
        private volatile bool busy;

        public MainForm()
        {
            Text = "Appdata Backup";
            StartPosition = FormStartPosition.CenterScreen;
            MinimumSize = new Size(640, 440);
            Size = new Size(720, 530);
            Font = new Font("Segoe UI", 9F);

            exeDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
            cfgPath = Path.Combine(exeDir, "AppdataBackup.cfg");
            config = BackupConfig.Load(cfgPath);
            LocateBandizip();
            config.Save(cfgPath);

            BuildUi();
            AppendLog("준비되었습니다. ZIP과 CFG는 EXE가 있는 폴더에 저장됩니다.");
            AppendLog(config.BandizipPath.Length > 0
                ? "Bandizip: " + config.BandizipPath
                : "Bandizip을 찾지 못했습니다. 작업 시작 시 다시 확인합니다.");
        }

        protected override void OnResize(EventArgs e)
        {
            base.OnResize(e);
            LayoutControls();
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (busy)
            {
                MessageBox.Show(this, "작업이 끝난 뒤 종료해 주세요.", Text,
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                e.Cancel = true;
                return;
            }
            base.OnFormClosing(e);
        }

        private void BuildUi()
        {
            GroupBox backupGroup = new GroupBox();
            backupGroup.Text = "백업";
            backupGroup.Name = "backupGroup";
            Controls.Add(backupGroup);

            GroupBox restoreGroup = new GroupBox();
            restoreGroup.Text = "복원";
            restoreGroup.Name = "restoreGroup";
            Controls.Add(restoreGroup);

            string[] backupText = { "Appdata 백업", "Stash 백업", "PersonalWiki 백업", "전부 백업" };
            string[] restoreText = { "Appdata 복원", "Stash 복원", "PersonalWiki 복원", "전부 복원" };
            Operation[] backupOps = {
                Operation.BackupAppdata, Operation.BackupStash, Operation.BackupWiki, Operation.BackupAll
            };
            Operation[] restoreOps = {
                Operation.RestoreAppdata, Operation.RestoreStash, Operation.RestoreWiki, Operation.RestoreAll
            };

            for (int i = 0; i < 4; i++)
            {
                buttons[i] = MakeButton(backupText[i], backupOps[i]);
                backupGroup.Controls.Add(buttons[i]);
                buttons[i + 4] = MakeButton(restoreText[i], restoreOps[i]);
                restoreGroup.Controls.Add(buttons[i + 4]);
            }

            buttons[8] = new Button();
            buttons[8].Text = "설정";
            buttons[8].Click += delegate { OpenSettings(); };
            Controls.Add(buttons[8]);

            statusLabel = new Label();
            statusLabel.Text = "준비";
            statusLabel.TextAlign = ContentAlignment.MiddleLeft;
            Controls.Add(statusLabel);

            logBox = new TextBox();
            logBox.Multiline = true;
            logBox.ReadOnly = true;
            logBox.ScrollBars = ScrollBars.Vertical;
            Controls.Add(logBox);

            LayoutControls();
        }

        private Button MakeButton(string text, Operation operation)
        {
            Button button = new Button();
            button.Text = text;
            button.Tag = operation;
            button.Click += delegate { StartTask((Operation)button.Tag); };
            return button;
        }

        private void LayoutControls()
        {
            if (buttons[0] == null || statusLabel == null || logBox == null) return;
            int margin = 18;
            int gap = 12;
            int width = ClientSize.Width;
            int height = ClientSize.Height;
            int groupWidth = Math.Max(220, (width - margin * 2 - gap) / 2);
            int groupY = 16;
            int groupH = 168;

            Control backupGroup = Controls["backupGroup"];
            Control restoreGroup = Controls["restoreGroup"];
            backupGroup.SetBounds(margin, groupY, groupWidth, groupH);
            restoreGroup.SetBounds(margin + groupWidth + gap, groupY, groupWidth, groupH);

            int buttonWidth = groupWidth - 32;
            for (int i = 0; i < 4; i++)
            {
                buttons[i].SetBounds(16, 28 + i * 32, buttonWidth, 27);
                buttons[i + 4].SetBounds(16, 28 + i * 32, buttonWidth, 27);
            }

            buttons[8].SetBounds(width - margin - 100, 195, 100, 30);
            statusLabel.SetBounds(margin, 201, width - margin * 2 - 112, 24);
            logBox.SetBounds(margin, 239, width - margin * 2, Math.Max(80, height - 257));
        }

        private void OpenSettings()
        {
            using (SettingsForm form = new SettingsForm(config))
            {
                if (form.ShowDialog(this) != DialogResult.OK) return;
                config = form.ToConfig();
                config.Save(cfgPath);
                AppendLog("설정을 저장했습니다. (Local 기준 " + config.LocalThresholdMiB.ToString("0.0") + " MiB)");
            }
        }

        private void StartTask(Operation operation)
        {
            if (busy) return;

            if (TouchesWiki(operation) && !IsAdministrator())
            {
                DialogResult answer = MessageBox.Show(this,
                    "이 작업은 C:\\Wiki에 접근하므로 관리자 권한이 필요할 수 있습니다.\r\n\r\n관리자 권한으로 앱을 다시 실행할까요?",
                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button1);
                if (answer == DialogResult.Yes) RelaunchAsAdministrator();
                return;
            }

            if (!LocateBandizip())
            {
                MessageBox.Show(this,
                    "Bandizip.exe를 찾지 못했습니다.\r\n\r\nBandizip을 설치하거나 설정에서 Bandizip.exe 경로를 지정해 주세요.",
                    Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            string archive = "";
            string prefix = PrefixForRestore(operation);
            if (prefix != null && !ChooseArchive(prefix, out archive)) return;

            if (IsRestore(operation))
            {
                string question = operation == Operation.RestoreAll
                    ? "각 종류의 최신 백업을 원래 위치에 복원합니다.\r\n같은 이름의 파일은 모두 덮어씁니다. 계속할까요?"
                    : "선택한 백업을 원래 위치에 복원합니다.\r\n같은 이름의 파일은 모두 덮어씁니다. 계속할까요?";
                if (MessageBox.Show(this, question, "복원 확인",
                    MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2) != DialogResult.Yes)
                    return;
            }

            SetBusy(true);
            AppendLog("----------------------------------------");
            Thread thread = new Thread(delegate()
            {
                bool ok = false;
                try
                {
                    ok = ExecuteOperation(operation, archive);
                }
                catch (Exception ex)
                {
                    Log("오류: " + ex.Message);
                }
                BeginInvoke(new Action(delegate { FinishTask(ok); }));
            });
            thread.IsBackground = true;
            thread.Start();
        }

        private bool ExecuteOperation(Operation operation, string archive)
        {
            bool ok = true;
            switch (operation)
            {
                case Operation.BackupAppdata:
                    return BackupOne("Appdata", config.AppdataLevel);
                case Operation.BackupStash:
                    return BackupOne("Stash", config.StashLevel);
                case Operation.BackupWiki:
                    return BackupOne("PersonalWiki", config.WikiLevel);
                case Operation.BackupAll:
                    ok = BackupOne("Appdata", config.AppdataLevel);
                    if (!BackupOne("Stash", config.StashLevel)) ok = false;
                    if (!BackupOne("PersonalWiki", config.WikiLevel)) ok = false;
                    return ok;
                case Operation.RestoreAppdata:
                    return RestoreOne("Appdata", archive);
                case Operation.RestoreStash:
                    return RestoreOne("Stash", archive);
                case Operation.RestoreWiki:
                    return RestoreOne("PersonalWiki", archive);
                case Operation.RestoreAll:
                    ok = RestoreLatest("Appdata");
                    if (!RestoreLatest("Stash")) ok = false;
                    if (!RestoreLatest("PersonalWiki")) ok = false;
                    return ok;
                default:
                    return false;
            }
        }

        private bool BackupOne(string prefix, int level)
        {
            string archive = MakeBackupPath(prefix);
            string temporary = archive + ".partial.zip";
            if (File.Exists(temporary)) File.Delete(temporary);

            string workingDir;
            List<string> items = new List<string>();
            if (StringEquals(prefix, "Appdata"))
            {
                workingDir = UserProfile();
                GatherAppdataItems(items);
            }
            else if (StringEquals(prefix, "Stash"))
            {
                workingDir = UserProfile();
                string source = Path.Combine(workingDir, ".stash");
                if (Directory.Exists(source)) items.Add(".stash");
            }
            else
            {
                workingDir = @"C:\";
                if (Directory.Exists(@"C:\Wiki")) items.Add("Wiki");
            }

            if (items.Count == 0)
            {
                Log(prefix + " 원본 폴더를 찾을 수 없습니다.");
                return false;
            }

            Log(prefix + " 백업 시작 -> " + Path.GetFileName(archive));
            bool ok = CreateArchiveFromItems(temporary, workingDir, items, level);
            if (ok)
            {
                Log("ZIP 무결성 검사 중...");
                ok = RunBandizipSimple("t", temporary, null, false);
            }
            if (ok)
            {
                if (File.Exists(archive)) File.Delete(archive);
                File.Move(temporary, archive);
            }
            else
            {
                if (File.Exists(temporary)) File.Delete(temporary);
                Log(prefix + " 백업 실패");
                return false;
            }

            Log(prefix + " 백업 완료: " + Path.GetFileName(archive));
            EnforceRetention(prefix);
            return true;
        }

        private bool RestoreOne(string prefix, string archive)
        {
            string destination;
            if (StringEquals(prefix, "PersonalWiki"))
            {
                destination = @"C:\Wiki";
            }
            else if (StringEquals(prefix, "Appdata"))
            {
                destination = Path.Combine(UserProfile(), "AppData");
            }
            else
            {
                destination = Path.Combine(UserProfile(), ".stash");
            }

            Log(prefix + " 복원 시작: " + Path.GetFileName(archive));
            Log(prefix + " 복원 위치: " + destination);
            bool ok = RunBandizipSimple("x", archive, destination, true);
            Log(ok ? prefix + " 복원 완료" : prefix + " 복원 실패");
            return ok;
        }

        private bool RestoreLatest(string prefix)
        {
            string archive = LatestBackup(prefix);
            if (archive == null)
            {
                Log(prefix + " 백업을 찾지 못했습니다.");
                return false;
            }
            return RestoreOne(prefix, archive);
        }

        private void GatherAppdataItems(List<string> items)
        {
            string profile = UserProfile();
            string roaming = Path.Combine(profile, @"AppData\Roaming");
            string localLow = Path.Combine(profile, @"AppData\LocalLow");
            string local = Path.Combine(profile, @"AppData\Local");
            if (Directory.Exists(roaming)) items.Add(@"AppData\Roaming");
            if (Directory.Exists(localLow)) items.Add(@"AppData\LocalLow");
            if (!Directory.Exists(local)) return;

            HashSet<string> excludedNames = BuildExcludedFolderSet();
            long limit = (long)(config.LocalThresholdMiB * 1024.0 * 1024.0 + 0.5);
            int included = 0;
            int excluded = 0;
            int excludedBySetting = 0;

            foreach (FileSystemInfo child in SafeEnumerate(local))
            {
                string relative = Path.Combine(@"AppData\Local", child.Name);
                if ((child.Attributes & FileAttributes.Directory) != 0)
                {
                    if (excludedNames.Contains(child.Name))
                    {
                        excluded++;
                        excludedBySetting++;
                        continue;
                    }
                    if ((child.Attributes & FileAttributes.ReparsePoint) != 0)
                    {
                        excluded++;
                        continue;
                    }
                    if (FolderSizeWithinLimit(child.FullName, limit))
                    {
                        items.Add(relative);
                        included++;
                    }
                    else
                    {
                        excluded++;
                    }
                }
                else
                {
                    items.Add(relative);
                }
            }

            Log("Local 1차 폴더: " + included.ToString() + "개 포함, " +
                excluded.ToString() + "개 제외 (설정 제외 " + excludedBySetting.ToString() +
                "개, 기준 " + config.LocalThresholdMiB.ToString("0.0") + " MiB)");
        }

        private HashSet<string> BuildExcludedFolderSet()
        {
            HashSet<string> set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            char[] separators = { ';', ',', '\r', '\n' };
            foreach (string raw in config.LocalExcludedFolders.Split(separators, StringSplitOptions.RemoveEmptyEntries))
            {
                string name = raw.Trim();
                if (name.Length > 0) set.Add(name);
            }
            return set;
        }

        private static IEnumerable<FileSystemInfo> SafeEnumerate(string folder)
        {
            try
            {
                return new DirectoryInfo(folder).EnumerateFileSystemInfos().ToArray();
            }
            catch
            {
                return new FileSystemInfo[0];
            }
        }

        private static bool FolderSizeWithinLimit(string folder, long limit)
        {
            long total = 0;
            Stack<string> pending = new Stack<string>();
            pending.Push(folder);

            while (pending.Count > 0)
            {
                string current = pending.Pop();
                foreach (FileSystemInfo child in SafeEnumerate(current))
                {
                    if ((child.Attributes & FileAttributes.ReparsePoint) != 0) continue;
                    if ((child.Attributes & FileAttributes.Directory) != 0)
                    {
                        pending.Push(child.FullName);
                    }
                    else
                    {
                        long size;
                        try { size = new FileInfo(child.FullName).Length; }
                        catch { return false; }
                        if (size > limit - total) return false;
                        total += size;
                    }
                }
            }
            return total <= limit;
        }

        private bool CreateArchiveFromItems(string archive, string workingDir, List<string> items, int level)
        {
            int start = 0;
            bool first = true;
            while (start < items.Count)
            {
                int end = start;
                int chars = 512 + archive.Length + config.BandizipPath.Length;
                while (end < items.Count)
                {
                    int add = items[end].Length * 2 + 4;
                    if (end > start && chars + add > 24000) break;
                    chars += add;
                    end++;
                }

                List<string> batch = items.GetRange(start, end - start);
                if (!RunArchiveBatch(first ? "c" : "a", archive, workingDir, batch, level)) return false;
                first = false;
                start = end;
            }
            return true;
        }

        private bool RunBandizipSimple(string verb, string archive, string outputDir, bool overwrite)
        {
            List<string> args = new List<string>();
            args.Add(verb);
            args.Add("-y");
            if (overwrite) args.Add("-aoa");
            if (outputDir != null) args.Add("-o:" + outputDir);
            args.Add(archive);
            return RunProcess(args, null);
        }

        private bool RunArchiveBatch(string verb, string archive, string workingDir, List<string> items, int level)
        {
            List<string> args = new List<string>();
            args.Add(verb);
            args.Add("-y");
            args.Add("-r");
            args.Add("-l:" + level.ToString());
            args.Add("-fmt:zip");
            args.Add(archive);
            args.AddRange(items);
            return RunProcess(args, workingDir);
        }

        private bool RunProcess(List<string> args, string workingDir)
        {
            ProcessStartInfo info = new ProcessStartInfo();
            info.FileName = config.BandizipPath;
            info.Arguments = JoinArguments(args);
            info.UseShellExecute = false;
            info.CreateNoWindow = false;
            if (!String.IsNullOrEmpty(workingDir)) info.WorkingDirectory = workingDir;

            try
            {
                using (Process process = Process.Start(info))
                {
                    process.WaitForExit();
                    if (process.ExitCode == 0) return true;
                    Log("Bandizip 종료 코드: " + process.ExitCode.ToString());
                    return false;
                }
            }
            catch (Win32Exception ex)
            {
                Log("Bandizip 실행 실패 (Windows 오류 " + ex.NativeErrorCode.ToString() + ")");
                return false;
            }
        }

        private static string JoinArguments(IEnumerable<string> args)
        {
            StringBuilder builder = new StringBuilder();
            foreach (string arg in args)
            {
                if (builder.Length > 0) builder.Append(' ');
                builder.Append(QuoteArgument(arg));
            }
            return builder.ToString();
        }

        private static string QuoteArgument(string arg)
        {
            if (arg.Length > 0 && arg.IndexOfAny(new[] { ' ', '\t', '\r', '\n', '"' }) < 0) return arg;
            StringBuilder builder = new StringBuilder();
            builder.Append('"');
            int backslashes = 0;
            foreach (char ch in arg)
            {
                if (ch == '\\')
                {
                    backslashes++;
                    continue;
                }
                if (ch == '"')
                {
                    builder.Append('\\', backslashes * 2 + 1);
                    builder.Append('"');
                    backslashes = 0;
                    continue;
                }
                builder.Append('\\', backslashes);
                backslashes = 0;
                builder.Append(ch);
            }
            builder.Append('\\', backslashes * 2);
            builder.Append('"');
            return builder.ToString();
        }

        private string MakeBackupPath(string prefix)
        {
            string stem = prefix + "_" + DateTime.Now.ToString("yyMMdd") + "-";
            int max = 0;
            foreach (string file in Directory.GetFiles(exeDir, stem + "*.zip"))
            {
                string name = Path.GetFileNameWithoutExtension(file);
                string number = name.Substring(stem.Length);
                int parsed;
                if (int.TryParse(number, out parsed) && parsed > max) max = parsed;
            }
            return Path.Combine(exeDir, stem + (max + 1).ToString() + ".zip");
        }

        private string LatestBackup(string prefix)
        {
            List<string> files = FindBackups(prefix);
            return files.Count == 0 ? null : files[files.Count - 1];
        }

        private void EnforceRetention(string prefix)
        {
            List<string> files = FindBackups(prefix);
            for (int i = 0; i + 3 < files.Count; i++)
            {
                try
                {
                    File.Delete(files[i]);
                    Log("오래된 백업 삭제: " + Path.GetFileName(files[i]));
                }
                catch
                {
                    Log("오래된 백업 삭제 실패: " + Path.GetFileName(files[i]));
                }
            }
        }

        private List<string> FindBackups(string prefix)
        {
            List<string> files = Directory.GetFiles(exeDir, "*.zip")
                .Where(path => IsBackupName(Path.GetFileName(path), prefix))
                .OrderBy(path => File.GetLastWriteTimeUtc(path))
                .ToList();
            return files;
        }

        private static bool IsBackupName(string name, string prefix)
        {
            string start = prefix + "_";
            if (!name.StartsWith(start, StringComparison.OrdinalIgnoreCase)) return false;
            if (!name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) return false;
            string middle = name.Substring(start.Length, name.Length - start.Length - 4);
            int dash = middle.IndexOf('-');
            if (dash != 6 || middle.Length < 8) return false;
            for (int i = 0; i < 6; i++) if (!Char.IsDigit(middle[i])) return false;
            for (int i = dash + 1; i < middle.Length; i++) if (!Char.IsDigit(middle[i])) return false;
            return true;
        }

        private bool LocateBandizip()
        {
            if (IsBandizipExe(config.BandizipPath)) return true;

            string[] candidates = {
                Path.Combine(exeDir, "Bandizip.exe"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), @"Bandizip\Bandizip.exe"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), @"Bandizip\Bandizip.exe")
            };
            foreach (string candidate in candidates)
            {
                if (IsBandizipExe(candidate))
                {
                    config.BandizipPath = candidate;
                    return true;
                }
            }
            config.BandizipPath = "";
            return false;
        }

        private static bool IsBandizipExe(string path)
        {
            return !String.IsNullOrEmpty(path) &&
                   File.Exists(path) &&
                   String.Equals(Path.GetFileName(path), "Bandizip.exe", StringComparison.OrdinalIgnoreCase);
        }

        private bool ChooseArchive(string prefix, out string archive)
        {
            archive = "";
            using (OpenFileDialog dialog = new OpenFileDialog())
            {
                dialog.Title = "복원할 " + prefix + " 백업 선택";
                dialog.InitialDirectory = exeDir;
                dialog.Filter = "ZIP 백업 (*.zip)|*.zip|모든 파일 (*.*)|*.*";
                if (dialog.ShowDialog(this) != DialogResult.OK) return false;
                if (!IsBackupName(Path.GetFileName(dialog.FileName), prefix))
                {
                    MessageBox.Show(this, "선택한 파일 이름이 해당 백업 종류와 맞지 않습니다.",
                        Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return false;
                }
                archive = dialog.FileName;
                return true;
            }
        }

        private void SetBusy(bool value)
        {
            busy = value;
            foreach (Button button in buttons)
                if (button != null) button.Enabled = !value;
            statusLabel.Text = value ? "작업 중... 잠시 기다려 주세요." : "준비";
        }

        private void FinishTask(bool ok)
        {
            SetBusy(false);
            string summary = ok ? "작업이 완료되었습니다." : "일부 또는 전체 작업이 실패했습니다. 로그를 확인하세요.";
            AppendLog(summary);
            MessageBox.Show(this, summary, Text, MessageBoxButtons.OK,
                ok ? MessageBoxIcon.Information : MessageBoxIcon.Warning);
        }

        private void Log(string line)
        {
            if (IsDisposed) return;
            BeginInvoke(new Action(delegate { AppendLog(line); }));
        }

        private void AppendLog(string line)
        {
            if (logBox == null) return;
            string stamped = "[" + DateTime.Now.ToString("HH:mm:ss") + "] " + line + "\r\n";
            logBox.AppendText(stamped);
        }

        private void RelaunchAsAdministrator()
        {
            try
            {
                ProcessStartInfo info = new ProcessStartInfo();
                info.FileName = Application.ExecutablePath;
                info.WorkingDirectory = exeDir;
                info.UseShellExecute = true;
                info.Verb = "runas";
                Process.Start(info);
                Close();
            }
            catch (Win32Exception ex)
            {
                if (ex.NativeErrorCode != 1223)
                    MessageBox.Show(this, "관리자 권한 재실행을 시작하지 못했습니다.",
                        Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private static bool IsAdministrator()
        {
            WindowsIdentity identity = WindowsIdentity.GetCurrent();
            WindowsPrincipal principal = new WindowsPrincipal(identity);
            return principal.IsInRole(WindowsBuiltInRole.Administrator);
        }

        private static bool TouchesWiki(Operation operation)
        {
            return operation == Operation.BackupWiki || operation == Operation.RestoreWiki ||
                   operation == Operation.BackupAll || operation == Operation.RestoreAll;
        }

        private static bool IsRestore(Operation operation)
        {
            return operation == Operation.RestoreAppdata || operation == Operation.RestoreStash ||
                   operation == Operation.RestoreWiki || operation == Operation.RestoreAll;
        }

        private static string PrefixForRestore(Operation operation)
        {
            if (operation == Operation.RestoreAppdata) return "Appdata";
            if (operation == Operation.RestoreStash) return "Stash";
            if (operation == Operation.RestoreWiki) return "PersonalWiki";
            return null;
        }

        private static string UserProfile()
        {
            return Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        }

        private static bool StringEquals(string left, string right)
        {
            return String.Equals(left, right, StringComparison.OrdinalIgnoreCase);
        }
    }

    internal sealed class SettingsForm : Form
    {
        private readonly ComboBox appdataLevel = new ComboBox();
        private readonly ComboBox stashLevel = new ComboBox();
        private readonly ComboBox wikiLevel = new ComboBox();
        private readonly TextBox threshold = new TextBox();
        private readonly TextBox exclusions = new TextBox();
        private readonly TextBox bandizip = new TextBox();

        public SettingsForm(BackupConfig config)
        {
            Text = "설정";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(405, 400);
            Font = new Font("Segoe UI", 9F);

            AddLabel("Appdata 압축", 24, 24, 125, 24);
            AddLabel("Stash 압축", 24, 64, 125, 24);
            AddLabel("PersonalWiki 압축", 24, 104, 125, 24);
            AddLabel("Local 폴더 임계값", 24, 153, 145, 24);
            AddLabel("Local 제외 폴더", 24, 193, 145, 24);
            AddLabel("(한 줄에 하나)", 24, 217, 125, 24);
            AddLabel("Bandizip 경로", 24, 270, 125, 24);

            InitCombo(appdataLevel, config.AppdataLevel, 170, 20);
            InitCombo(stashLevel, config.StashLevel, 170, 60);
            InitCombo(wikiLevel, config.WikiLevel, 170, 100);

            threshold.Text = config.LocalThresholdMiB.ToString("0.0");
            threshold.SetBounds(170, 148, 120, 28);
            Controls.Add(threshold);
            AddLabel("MiB", 300, 153, 50, 24);

            exclusions.Multiline = true;
            exclusions.ScrollBars = ScrollBars.Vertical;
            exclusions.Text = config.LocalExcludedFolders.Replace(";", "\r\n");
            exclusions.SetBounds(170, 188, 190, 64);
            Controls.Add(exclusions);

            bandizip.Text = config.BandizipPath;
            bandizip.SetBounds(24, 298, 276, 28);
            Controls.Add(bandizip);

            Button browse = new Button();
            browse.Text = "찾아보기";
            browse.SetBounds(308, 296, 76, 31);
            browse.Click += delegate { BrowseBandizip(); };
            Controls.Add(browse);

            Button save = new Button();
            save.Text = "저장";
            save.DialogResult = DialogResult.None;
            save.SetBounds(194, 349, 90, 32);
            save.Click += delegate { SaveAndClose(); };
            Controls.Add(save);
            AcceptButton = save;

            Button cancel = new Button();
            cancel.Text = "취소";
            cancel.DialogResult = DialogResult.Cancel;
            cancel.SetBounds(294, 349, 90, 32);
            Controls.Add(cancel);
            CancelButton = cancel;
        }

        public BackupConfig ToConfig()
        {
            BackupConfig config = new BackupConfig();
            config.AppdataLevel = SelectionToLevel(appdataLevel.SelectedIndex);
            config.StashLevel = SelectionToLevel(stashLevel.SelectedIndex);
            config.WikiLevel = SelectionToLevel(wikiLevel.SelectedIndex);
            config.LocalThresholdMiB = Double.Parse(threshold.Text);
            config.LocalExcludedFolders = NormalizeExclusions(exclusions.Text);
            config.BandizipPath = bandizip.Text.Trim();
            return config;
        }

        private void InitCombo(ComboBox combo, int level, int x, int y)
        {
            combo.DropDownStyle = ComboBoxStyle.DropDownList;
            combo.Items.Add("빠름 (1)");
            combo.Items.Add("균형 (4)");
            combo.Items.Add("압축률 (9)");
            combo.SelectedIndex = LevelToSelection(level);
            combo.SetBounds(x, y, 190, 28);
            Controls.Add(combo);
        }

        private void AddLabel(string text, int x, int y, int width, int height)
        {
            Label label = new Label();
            label.Text = text;
            label.TextAlign = ContentAlignment.MiddleLeft;
            label.SetBounds(x, y, width, height);
            Controls.Add(label);
        }

        private void BrowseBandizip()
        {
            using (OpenFileDialog dialog = new OpenFileDialog())
            {
                dialog.Title = "Bandizip 실행 파일 선택";
                dialog.Filter = "Bandizip 실행 파일 (Bandizip.exe)|Bandizip.exe|실행 파일 (*.exe)|*.exe";
                if (dialog.ShowDialog(this) == DialogResult.OK) bandizip.Text = dialog.FileName;
            }
        }

        private void SaveAndClose()
        {
            double value;
            if (!Double.TryParse(threshold.Text, out value) || value < 0.1 || value > 1048576.0)
            {
                MessageBox.Show(this, "임계값을 0.1 이상인 MiB 숫자로 입력하세요.",
                    Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            string path = bandizip.Text.Trim();
            if (!File.Exists(path) || !String.Equals(Path.GetFileName(path), "Bandizip.exe", StringComparison.OrdinalIgnoreCase))
            {
                MessageBox.Show(this, "유효한 Bandizip.exe 경로를 선택하세요.",
                    Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            DialogResult = DialogResult.OK;
            Close();
        }

        private static int LevelToSelection(int level)
        {
            return level == 1 ? 0 : level == 4 ? 1 : 2;
        }

        private static int SelectionToLevel(int selection)
        {
            return selection == 0 ? 1 : selection == 1 ? 4 : 9;
        }

        private static string NormalizeExclusions(string value)
        {
            string[] lines = value.Split(new[] { '\r', '\n', ';', ',' }, StringSplitOptions.RemoveEmptyEntries);
            List<string> names = new List<string>();
            foreach (string raw in lines)
            {
                string name = raw.Trim();
                if (name.Length > 0) names.Add(name);
            }
            return String.Join(";", names.ToArray());
        }
    }

    internal static class Program
    {
        [STAThread]
        private static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }
}
