# Appdata Backup

Bandizip의 `Bandizip.exe`를 사용해 AppData, `.stash`, `C:\Wiki`를 백업하고 복원하는 Windows GUI 앱입니다.

- 백업 파일과 설정 파일은 `AppdataBackup.exe`와 같은 폴더에 저장됩니다.
- AppData의 `Roaming`, `LocalLow`는 전부 백업합니다.
- `Local`은 바로 아래 폴더 중 설정한 크기 이하인 폴더와 바로 아래의 일반 파일을 백업합니다.
- 백업 종류별로 최근 ZIP 3개만 유지합니다.
- 복원 시 같은 이름의 파일은 항상 덮어씁니다.
- 개별 복원은 ZIP을 직접 선택하며, 전부 복원은 종류별 최신 ZIP을 사용합니다.

Bandizip 설치 경로의 `Bandizip.exe`를 자동 탐색합니다. 설정 창에서 경로를 직접 지정할 수도 있습니다.

## 기본 설정

- Appdata: 균형 (4)
- Stash: 빠름 (1)
- PersonalWiki: 압축률 (9)
- AppData Local 폴더 임계값: 25.6 MiB
