# 실행 안내

이 배포본은 [HyRUS_EnvData_Router](https://github.com/VIPSfef/HyRUS_EnvData_Router)의 호스트 MAVLink 수신·분석·서버 전송 프로그램입니다. I²C 센서 읽기와 PX4 MAVLink 발행·펌웨어는 별도 `HyRUS_EnvData_Px4`에서 관리합니다. 입력 메시지·단위·시각은 [PX4 입력 규약](PX4_MAVLINK_CONTRACT.md)을 참고하세요.

압축 파일 전체를 새 폴더에 풉니다. `bin`, `tools`, `config`, `docs`, `licenses`를 함께 보관하세요.

## Windows

Windows 10/11 x64용입니다. Windows 기본 시스템 DLL과 UCRT를 사용하며 별도 MinGW 설치가 필요하지 않습니다.

1. `Start-Router.cmd`를 실행합니다. 처음에만 깨끗한 설정 예제를 같은 폴더의 `telemetry.ini`로 복사합니다.
2. `e`로 설정을 열고 `url`에 센서 서버의 전체 주소를 입력합니다. 예: `http://127.0.0.1:21000/ingest`.
3. 인증이 필요하면47번 `token`에 입력합니다. `Enter` 후 `r`로 저장·적용합니다. 입력은 가려지며 Windows 사용자별로 암호화 저장됩니다.

기존 설정 파일은 실행 스크립트가 덮어쓰지 않습니다. 다른 PC·Windows 사용자에게 운용 설정을 복사할 때는 토큰을 다시 입력하세요.

## Linux

배포 바이너리 기준은 Ubuntu24.04/WSL x64, glibc2.39와 `libcurl.so.4`입니다. 필요한 경우 `sudo apt install libcurl4t64`를 실행하세요.

```sh
./start-router.sh
```

설정은 `$XDG_CONFIG_HOME/mavlink_telemetry_data_router/telemetry.ini`, 변수가 없으면 `$HOME/.config/mavlink_telemetry_data_router/telemetry.ini`에 생성됩니다. WSL에서는 이 설정 경로를 Linux 파일시스템에 두세요. 토큰은0600 권한 설정 파일에 평문 저장됩니다. 시리얼 장치에 접근할 사용자 권한이 필요합니다.

## 시험 서버와 전송

Windows는 `Start-Test-Server.cmd`, Linux는 `./start-test-server.sh`를 실행합니다. 시험 서버는 같은 PC의 HTTP `127.0.0.1:21000`, UDP `127.0.0.1:21001`에서 기다립니다. 이 스크립트는 라우터의 목적지를 바꾸지 않습니다.

초기 설정 `url=-`는 HTTP 송신이 꺼진 상태입니다. 시험 서버를 쓰려면 라우터의 `url`을 `http://127.0.0.1:21000/ingest`로 바꾸세요. `udp_backup_enabled=1`이면 같은 호스트의 TCP 포트+1로 UDP 백업을 보냅니다. 필요하면 UDP 호스트·포트를 따로 지정할 수 있습니다. 토큰은 HTTP 인증에만 사용합니다.

새 표본이 없어도 연결·센서 상태를10초마다 받으려면 `status_interval_ms=10000`으로 설정합니다. `mavlink_rates_enabled=1`은 기체의 임시 메시지 전송률 조절을 활성화합니다. 두 기능은 기본값에서 꺼져 있습니다.

QGC는 별도의 MAVLink UDP `127.0.0.1:14550`을 사용합니다. 센서 서버의 URL과 다른 통신입니다. 시리얼은 존재하는 포트를 기본115200baud부터 자동 탐색합니다.

HTTP 대기열은 기본1GiB RAM 상한이며 `tcp_buffer_max_mb`로 바꿉니다. HTTP204 확인 전 자료를 보관하므로 전송이 밀린 상태에서는 프로세스를 강제로 종료하지 마세요. 프로세스 종료 시 메모리 자료는 사라집니다.

`http_min_interval_ms=500`은 측정·상태·재시도를 합친 HTTP 요청의 최소 완료 후 간격입니다. 기본값에서 초당 최대2회 수준으로 분산하며, 응답 시간이 길면 더 낮아집니다. 401·403·429에는1초부터 최대30초까지 재시도 대기를 늘립니다. HTTP 대기 중에도 수신과 UDP 백업은 계속됩니다. 403이 반복되면 서버 로그에서 인증·권한·요청 제한 중 실제 거부 원인을 확인하세요.

`q`는 수집을 종료하지 않습니다. 종료는 `Ctrl+C`를 사용합니다. `r`는 저장한 설정으로 수집기를 다시 시작합니다. 자세한 규약은 [서버 API](SERVER_API.md), [UDP 설명](UDP_BACKUP.md)을 확인하세요.

프로젝트 전체의 배포 라이선스는 아직 지정되지 않았습니다. 포함된 제3자 고지는 `licenses`에 있습니다. 버전·파일 무결성은 `--version`과 `SHA256SUMS.txt`로 확인할 수 있습니다.
