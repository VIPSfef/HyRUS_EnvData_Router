# 배포 폴더

현재 배포 버전은 **0.11.0**입니다. 소스는 [HyRUS_EnvData_Router](https://github.com/VIPSfef/HyRUS_EnvData_Router), 다운로드는 그 저장소의 [Releases](https://github.com/VIPSfef/HyRUS_EnvData_Router/releases)에서 관리합니다. 생성한 실행 파일·압축 파일은 Git 소스 이력에서 제외하고, 아래 문서와 패키징 스크립트로 다시 만듭니다.

배포 범위는 호스트 MAVLink 수신·분석·서버 전송 프로그램과 검증용 시험 서버입니다. PX4 I²C 드라이버·MAVLink 발행·펌웨어 패치는 별도 `HyRUS_EnvData_Px4`에서 관리하며 이 배포물에 포함하지 않습니다. 캐리어 규약은 [PX4 → Router 입력 규약](../docs/PX4_MAVLINK_CONTRACT.md)을 따릅니다.

```text
release/
  README.md
  v0.11.0/
    windows-x64/
      Start-Router.cmd
      Start-Test-Server.cmd
      bin/                 headless·interactive 실행 파일
      tools/               시험 서버
      config/              telemetry.ini.example
      docs/                서버 API·규격·릴리스 노트
      licenses/            제3자 라이선스 고지
    linux-x64/
      start-router.sh
      start-test-server.sh
      bin/
      tools/
      config/
      docs/
      licenses/
    assets/                전달·GitHub Release 첨부용 파일
      mavlink_telemetry_data_router-0.11.0-windows-x64.zip
      mavlink_telemetry_data_router-0.11.0-linux-x64.tar.gz
      mavlink_telemetry_data_router-0.11.0-source.zip
      SHA256SUMS.txt
      manifest.json
```

## 받은 프로그램 실행하기

Windows ZIP 또는 Linux TAR.GZ를 **전체 압축 해제**한 뒤 사용합니다. `bin`의 실행 파일만 따로 옮기지 마세요. Windows는 `Start-Router.cmd`를 두 번 클릭하고, Linux는 압축을 푼 폴더에서 `./start-router.sh`를 실행합니다. headless와 interactive 중 하나만 실행해야 같은 COM/tty 포트를 사용할 수 있습니다.

최초 설정은 `config/telemetry.ini.example`을 바탕으로 준비합니다. 실제 서버 주소·토큰과 관측 데이터는 배포 파일에 넣지 않습니다. 기본 예제의 `url=-`는 서버로 보내지 않는 미리보기 모드입니다. TUI의 `e`에서 전체 서버 URL을 입력하고, 필요한 경우47번 `token`에 인증값을 입력한 뒤 `r`로 저장·적용합니다. HTTP 주소에 포트와 경로를 함께 적습니다.

로컬 수신 확인은 `Start-Test-Server.cmd` 또는 `./start-test-server.sh`로 시험 서버를 켜고 라우터 URL을 `http://127.0.0.1:21000/ingest`로 설정합니다. UDP 백업의 기본 목적지는 같은 호스트의21001입니다. 시험 서버는 화면 확인용이며 영속 데이터베이스를 제공하지 않습니다.

새 표본이 없어도 연결 상태를 확인하려면 `status_interval_ms=10000`, UDP 수신까지 시험하려면 `udp_backup_enabled=1`로 설정하고 `r`로 적용합니다. 배포 예제에서는 두 기능이 기본적으로 꺼져 있습니다.

HTTP는 `http_min_interval_ms=500`을 기본으로 측정·상태·재시도 요청 전체를 초당 최대2회 수준으로 분산합니다. 각 요청 완료 뒤 간격을 지키며 밀린 측정 자료는 RAM에 보관합니다. 401·403·429에는 최대30초까지 대기를 늘리고, UDP 백업은 이 대기와 독립적으로 동작합니다. 403의 실제 원인은 서버 로그에서 확인합니다.

운용 중인 설정은 새 배포본으로 덮어쓰지 마세요. 대기 데이터는 RAM에만 보관하므로 업데이트를 위해 프로세스를 종료하기 전에 미확인 대기열이 비었는지 확인합니다. 종료하면 남은 대기 데이터는 사라집니다.

## 실행 환경

Windows x64 배포본은 Windows의 UCRT와 시스템 DLL을 사용합니다. 별도 MinGW 런타임 DLL은 필요하지 않습니다. 이 배포본은32비트 Windows용이 아닙니다.

Linux x64 배포본은 **Ubuntu24.04 WSL, glibc2.39** 환경에서 빌드·확인합니다. 동적 라이브러리 `libcurl.so.4`가 필요하며 Ubuntu24.04에서는 `libcurl4t64` 패키지가 제공합니다. 더 오래된 glibc 환경과의 호환성은 확인하지 않았습니다. 다른 배포판·이전 환경에는 해당 환경에서 소스를 빌드하세요.

## 첨부 파일과 무결성 확인

`assets/`에는 Windows ZIP, Linux TAR.GZ, 소스 ZIP, `SHA256SUMS.txt`, `manifest.json`을 둡니다. 소스 ZIP에는 Git 내부 이력, 운용 설정, 토큰, 수집 자료와 빌드 결과가 들어가지 않습니다. manifest는 배포 버전·소스 상태·산출물 정보를 기록합니다.

Linux에서는 `assets/`에서 다음 명령으로 파일 해시를 확인합니다.

```sh
sha256sum -c SHA256SUMS.txt
```

Windows PowerShell에서는 같은 폴더에서 `Get-FileHash -Algorithm SHA256 .\파일이름.zip`의 결과를 `SHA256SUMS.txt`의 해당 줄과 비교합니다.

## GitHub 게시 준비

GitHub Release의 본문은 [0.11.0 릴리스 노트](../docs/releases/v0.11.0.md)를 사용하고 `assets/` 파일을 첨부합니다. 생성한 OS별 실행 폴더 자체를 Git에 커밋하지 않습니다. 게시 대상은 `VIPSfef/HyRUS_EnvData_Router`이며, 게시할 커밋·태그와 manifest의 소스 상태를 맞춘 뒤 [협업 문서](../docs/COLLABORATION.md)의 절차를 진행하세요.

프로젝트 자체의 라이선스 선택은 별도 결정 사항입니다. `licenses/`에는 포함한 제3자 구성요소의 고지를 보관합니다.
