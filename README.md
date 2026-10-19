# mavlink_telemetry_data_router

**MAVLink 스트림을 자동으로 찾아 관측값을 수집하고, 메모리에 모았다가 서버로 보내는 상시 중계 프로그램**입니다. Windows x64와 Linux x64를 지원합니다.

Windows의 COM, Linux의 tty 또는 설정한 WebSocket에서 MAVLink v1/v2를 직접 읽습니다. 매 관측을 사람이 입력하거나 다른 CSV 변환기를 실행할 필요가 없습니다.

## 어떤 파일을 실행하나요?

| 프로그램 | 용도 |
|---|---|
| `mavlink_telemetry_data_router_interactive` | 기기 목록·수신 상태·서버 전송 상태를 보며 설정을 편집할 때 |
| `mavlink_telemetry_data_router_headless` | 화면 없이 계속 수집·중계할 때 |

Windows에는 `.exe`가 붙습니다. **둘 중 하나만 실행**하세요. interactive 자체가 수신·처리·전송을 수행합니다.

실행 파일은 `build/windows-x64/` 또는 `build/linux-x64/`에 각각 두 개씩 있습니다. 배포 ZIP/TAR.GZ 안에서는 `bin/`에 있습니다. 소스는 `src/`, 선택한 개발 검사 결과는 `build/<OS>/tests/`, 배포물은 `build/packages/`에 둡니다.

## 먼저 설정하고 실행하기

아래 명령은 소스 폴더 기준입니다. 설정 예제를 복사합니다.

Windows PowerShell:

```powershell
Copy-Item .\config\telemetry.ini.example .\telemetry.ini
.\build\windows-x64\mavlink_telemetry_data_router_interactive.exe --config .\telemetry.ini
```

Linux:

```sh
cp config/telemetry.ini.example telemetry.ini
./build/linux-x64/mavlink_telemetry_data_router_interactive --config ./telemetry.ini
```

관측 스트림은 프로그램이 자동으로 읽습니다. 위 명령 뒤에 데이터를 파이프로 넣지 않습니다.

설정 파일을 지정하지 않으면 현재 폴더의 `telemetry.ini`, 그다음 실행 파일 옆의 `telemetry.ini`를 찾습니다. 둘 다 없으면 실행 파일 옆에 기본 설정을 만들고 자동 탐색을 시작합니다.

**예제의 `url=-`는 JSON 미리보기 모드이며 서버 전송이 아닙니다.** 실제 운용 전에 `url`을 서버 주소로 바꾸세요. 미리보기 JSON과 화면을 함께 보고 싶지 않다면 Windows에서는 명령 뒤에 `1>$null`, Linux에서는 `>/dev/null`을 붙입니다.

## 어떤 설정을 바꾸면 되나요?

```ini
input_mode=mavlink
serial_port=auto
serial_baud=115200
websocket_url=
auto_scan=1
url=https://YOUR_SERVER/ingest
stream=weather
session=flight-a
send_interval_ms=1000
origin_auto=1
unit_map=temp:C;humidity:%;pm25:ug/m3
```

| 설정 | 의미 |
|---|---|
| `serial_port` | Windows `COM5`, Linux `/dev/ttyACM0`처럼 우선 확인할 포트. `auto`는 자동 탐색, `none`은 우선 포트 없음 |
| `serial_baud` | 장치와 일치하는 통신 속도. 기본 115200 |
| `websocket_url` | 선택 사항. `ws://HOST/path` 또는 `wss://HOST/path`의 **바이너리 MAVLink** 스트림 |
| `auto_scan` | `1`이면 다른 COM/tty도 검색. `0`이면 지정한 포트·WebSocket만 사용 |
| `url` | 관측을 전송할 HTTP/HTTPS 서버 주소 |
| `stream`, `session` | 장치 흐름과 관측 실행을 구분하는 이름. 실제 송신 세션에는 실행·소스 변경 식별자가 붙음 |
| `origin_auto` | 기본 `1`: 첫 유효 전역 항법 위치를 지역 좌표 원점으로 사용. `0`: 지정한 원점 사용 |
| `unit_map` | 선택 사항. 키에 단위를 부여하는 `키:단위;키:단위` 목록 |

인증 토큰 값은 `token_env`에 지정한 환경변수에 넣습니다. 기본 환경변수 이름은 `TB_TOKEN`입니다. 설정 파일에는 토큰 값 대신 환경변수 이름을 저장합니다.

interactive에서 `e`로 설정을 열고 항목 편집 후 Enter로 확인, `s`로 저장합니다. **저장은 재시작 후 적용**됩니다. 미확인 관측이 남았다면 먼저 전송을 마친 뒤 재시작하세요.

## 실행 후 자동으로 하는 일

1. 설정한 포트와 WebSocket을 우선 확인하고, 필요하면 다른 COM/tty를 순서대로 탐색합니다.
2. CRC가 맞는 MAVLink를 발견하면 해당 채널을 유지합니다. 데이터가 끊기면 다시 탐색합니다.
3. HEARTBEAT에서 시스템·컴포넌트 ID, 기기 종류와 PX4/ArduPilot 등의 정보를 확인해 목록으로 보여 줍니다.
4. 선택한 비행제어기의 시계에 속한 키–값 관측에 위치·속도·부팅 경과시각을 연결합니다.
5. 비어 있지 않은 관측만 원본·통계와 함께 묶어 기본 1초 간격으로 서버에 보냅니다.
6. 서버가 확정 수락한 묶음에 HTTP 204로 응답하면 해당 메모리 공간을 재사용합니다.

관측은 **메모리에만** 유지합니다. 서버에 연결되지 않아도 수신은 계속하지만 메모리가 가득 차면 새 묶음을 버리고 손실을 표시합니다. 종료 시 남은 미확인 자료는 소실됩니다.

하트비트·항법만 있고 수집할 관측값이 없으면 서버에 빈 요청을 보내지 않습니다. 요청이 느리거나 실패하면 재시도하므로 정확한 정시 도착이나 무조건 초당 한 번의 성공을 보장하지는 않습니다.

## 어떤 데이터를 수집하나요?

기본 키–값 운반 형식은 MAVLink `NAMED_VALUE_FLOAT`와 `NAMED_VALUE_INT`입니다. **온도·습도·미세먼지 같은 항목을 코드에 고정하지 않고 새 키를 자동 등록**합니다. 이름이 추가됐다는 이유로 프로그램을 다시 만들 필요가 없습니다. 현재 한 소스 세션의 키 한도는 16개이며 초과는 거부 수로 표시합니다.

이 메시지에는 단위가 없으므로 `unit_map`에서 지정합니다. 미지정 단위는 `unknown`으로 보존합니다. 기본 압력·온도 메시지와 유효 필드가 표시된 HIGHRES_IMU도 수집합니다.

MAVLink 패킷 CRC·하트비트는 직접 검사합니다. 알 수 없는 메시지는 미해독 수로 표시하고 다른 메시지 수신을 계속합니다. XML이 다른 임의의 새로운 메시지를 내용 정의 없이 추측해 해독하지는 않습니다.

다른 컴포넌트의 경과시각을 비행제어기 시각과 임의로 섞지 않습니다. 기기 목록에는 표시하되, 시계 관계가 확인되지 않은 관측은 거부 사유를 표시합니다.

## 위치·시각과 통계를 어떻게 보존하나요?

원본에는 키·단위·값과 그 관측의 부팅 시각·위치·속도·품질이 남습니다. 시각이 붙은 실제 항법 사이에서만 위치를 보간하며, 위치를 연결할 수 없는 관측은 보류한 뒤 사유와 함께 누락 수로 표시합니다.

첫 전역 항법 위치를 자동 원점으로 사용할 수 있습니다. 실제 MAVLink 항법의 고도 기준은 MSL이며 타원체고로 바꾸어 표기하지 않습니다. 위치 오차를 모르면 JSON `null`로 보냅니다.

`SYSTEM_TIME`의 원시 boot/UTC 대응점도 보존합니다. 그 오차를 알 수 없는 기본 상태에서는 관측별 UTC 추정은 미상으로 남깁니다. `system_time_uncert_us`에 검증한 오차를 지정하면 UTC 추정과 불확실도를 함께 계산합니다.

1초마다 보내는 묶음과 측정시각의 1초 구간은 다릅니다. 같은 측정 구간이 여러 전송 묶음으로 나뉠 수 있어도, 원본 시각은 바꾸지 않습니다. 서버는 `schema:3`의 **`(stream, session, batch_id)`로 중복을 제거**하고 같은 구간의 자료를 합쳐야 합니다. 통계만으로 이동 경로의 원본을 완전히 복원할 수 없으므로 기본값은 원본 포함입니다.

## 화면과 종료

- `d`: 하트비트로 발견한 장치 목록
- `k`: 관측값과 전체 진단
- 위/아래: 목록 이동
- `e`: 설정 편집, Enter: 검증, `s`: 저장
- Ctrl+C: 수신 종료 후 대기 자료 전송을 시도하고 종료

서버 오류·CRC 오류·미해독 메시지·위치 없음·시계 불일치·지연 관측·큐 손실을 구분해 확인할 수 있습니다. 서버는 모든 묶음을 확정 수락한 뒤 204를 보내야 합니다. TCP ACK나 HTTP 200/202는 이 확인을 대신하지 않습니다.

## 빌드·설치

CMake 3.21 이상, Ninja, 대상 OS의 x64 C 컴파일러가 필요합니다. Windows는 LLVM-MinGW x64 Clang을 PATH에 두고 실행합니다.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
cmake --build --preset package-windows-x64
```

Linux에서는 회사 GitLab 등의 실제 clone URL을 사용합니다.

```sh
sudo apt-get update
sudo apt-get install git cmake ninja-build build-essential libcurl4-openssl-dev python3
git clone YOUR_REPOSITORY_CLONE_URL mavlink_telemetry_data_router
cd mavlink_telemetry_data_router
cmake --preset linux-x64
cmake --build --preset linux-x64
sh install.sh
```

기본 설치 위치는 `$HOME/.local/bin`이며 `PREFIX=/원하는/경로 sh install.sh`로 변경합니다. Windows에서 Linux x64도 [교차 컴파일](docs/REFERENCE.md#빌드와-개발-검사)할 수 있습니다.

## test_ 프로그램은 무엇인가요?

개발자가 수정 후 오류를 찾는 도구이며 운용 중 실행하거나 배포할 필요가 없습니다. 검사 코드는 운용 프로그램에 들어가지 않습니다.

| 검사 | 확인 대상 |
|---|---|
| `test_telemetry` | 통계·관측 순서·용량·보간·JSON |
| `test_time_sync` | UTC 추정·드리프트·극단 시각 |
| `test_config` | 설정 검증·저장·한글 경로·비밀정보 표시 방지 |
| `test_mavlink_input` | 실제 바이너리 패킷 CRC·장치 식별·위치 연결·재부팅 |
| `test_input_transport` | WebSocket·분할 프레임·ping/pong·가상 tty |

원인을 구분하기 위해 나눈 검사들입니다. **기본 빌드는 검사 OFF**이며, 선택하면 `tests/` 하위 폴더에 만듭니다. CTest는 위 검사와 실제 MAVLink→HTTP 통합 시험을 실행합니다.

```powershell
cmake --preset windows-x64-tests
cmake --build --preset windows-x64-tests
ctest --preset windows-x64-tests
```

Linux에서는 `windows`를 `linux`로 바꿉니다. 일반 프리셋으로 다시 구성하면 검사는 OFF가 되지만 이미 생성된 `tests/` 파일은 자동 삭제하지 않습니다.

## 상세 자료와 검증 범위

- [기술 참조](docs/REFERENCE.md): 전체 설정·좌표/시각·출력 규약·교차 빌드·CSV 재생
- [검토·검증 기록](docs/REVIEW.md): 실제 시험과 계산량·메모리·한계

Windows에서는 실제 바이너리 MAVLink를 WebSocket으로 받아 HTTP 서버에 전송하는 자동 시험을 통과했습니다. Linux x64는 실제 교차 빌드했으며 Linux에서의 직접 실행과 실제 Pixhawk/물리 COM 연결은 아직 검증하지 않았습니다.
