# mavlink_telemetry_data_router

**MAVLink 스트림을 자동으로 찾아 관측값을 수집하고, 메모리에 모았다가 서버로 보내는 상시 중계 프로그램**입니다. Windows x64와 Linux x64를 지원합니다.

현재 호스트 버전은 **0.9.0**입니다. Windows/Linux의 `_headless`·`_interactive` 네 실행 파일 이름은 유지하며, 이번 ESTGPS·화면 개선에 Pixhawk 펌웨어 변경은 필요하지 않습니다.

Windows의 COM, Linux의 tty, UDP 또는 설정한 WebSocket에서 MAVLink v1/v2를 직접 읽습니다. 매 관측을 사람이 입력하거나 다른 CSV 변환기를 실행할 필요가 없습니다.

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
udp_enabled=1
udp_bind=127.0.0.1
udp_port=14540
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
| `udp_enabled` | `1`이면 UDP 입력 사용, `0`이면 제외 |
| `udp_bind`, `udp_port` | 수신할 **로컬** IPv4 주소와 포트. 기본 `127.0.0.1:14540` |
| `websocket_url` | 선택 사항. `ws://HOST/path` 또는 `wss://HOST/path`의 **바이너리 MAVLink** 스트림 |
| `auto_scan` | `1`이면 다른 COM/tty도 검색. `0`이면 다른 COM/tty 자동 검색 제외. 지정 포트·WebSocket·활성 UDP는 유지 |
| `url` | 관측을 전송할 HTTP/HTTPS 서버 주소 |
| `stream`, `session` | 장치 흐름과 관측 실행을 구분하는 이름. 실제 송신 세션에는 실행·소스 변경 식별자가 붙음 |
| `origin_auto` | 기본 `1`: 첫 유효 전역 항법 위치를 지역 좌표 원점으로 사용. `0`: 지정한 원점 사용 |
| `unit_map` | 선택 사항. 키에 단위를 부여하는 `키:단위;키:단위` 목록 |

인증 토큰 값은 `token_env`에 지정한 환경변수에 넣습니다. 기본 환경변수 이름은 `TB_TOKEN`입니다. 설정 파일에는 토큰 값 대신 환경변수 이름을 저장합니다.

interactive에서 `e`로 설정을 열고 항목 편집 후 Enter로 확인, `s`로 저장합니다. **저장은 재시작 후 적용**됩니다. 미확인 관측이 남았다면 먼저 전송을 마친 뒤 재시작하세요.

## 서버 전송 연결만 먼저 시험하기 — TCP 21000

별도 시험 서버를 실행하면 하트비트·좌표가 서버까지 도착하는지 화면에서 볼 수 있습니다. **입력은 기존 COM/tty·UDP 14540이고, 서버 전송은 HTTP/TCP 21000**입니다.

Windows 첫 번째 터미널에서 서버를 실행합니다.

```powershell
.\build\windows-x64\tools\mavlink_telemetry_data_router_test_server.exe
```

두 번째 터미널에서 수집기를 시험 모드로 실행합니다.

```powershell
.\build\windows-x64\mavlink_telemetry_data_router_interactive.exe --test-heartbeat
```

Linux에서도 두 터미널을 사용합니다.

```sh
./build/linux-x64/tools/mavlink_telemetry_data_router_test_server
```

```sh
./build/linux-x64/mavlink_telemetry_data_router_interactive --test-heartbeat
```

기본 서버 주소는 `http://127.0.0.1:21000/ingest`입니다. 다른 컴퓨터의 서버라면 서버에 `--bind 0.0.0.0`을 지정하고 수집기에 `--server-url http://SERVER_IP:21000/ingest`를 추가합니다. 필요하면 기존 설정을 `--config telemetry.ini --test-heartbeat`로 지정합니다. CLI 주소는 해당 실행에 적용하며 설정 파일을 자동으로 덮어쓰지 않습니다.

`--test-heartbeat` 모드는 새 하트비트와 최신 유효 항법 좌표만 기본 1초 간격으로 보냅니다. 온습도·미세먼지·관측 통계는 보내지 않습니다. 위치가 없거나 오래되면 `position: null`로 표시하고 좌표를 만들어 내지 않습니다. 새 하트비트가 없으면 빈 요청을 보내지 않습니다.

시험 서버는 화면과 메모리 로그만 유지하며 파일에 저장하지 않습니다. `--plain`은 색상 없이 JSON 한 줄씩 표시합니다. 여기서 HTTP 204는 시험 서버가 내용을 검사하고 표시했다는 뜻입니다. 회사 서버의 영속 저장을 시험한 결과는 아닙니다. Ctrl+C로 각각 종료합니다.

시험 서버는 선택 도구로 `tools/`에 둡니다. 파일이 없다면 아래처럼 한 번 선택 빌드합니다. 기본 빌드의 운용 프로그램은 OS별 headless·interactive 두 개 그대로입니다.

```powershell
cmake --preset windows-x64 -DBUILD_TEST_SERVER=ON
cmake --build --preset windows-x64
```

Linux에서는 프리셋 이름을 `linux-x64`로 바꿉니다. 현재 WSL SITL을 시험할 때는 Linux 서버와 수집기를 같은 WSL에서 실행해야 내부 UDP 14540을 바로 받을 수 있습니다.

## 온습도 관측을 시험하기

**측정시각이 있는 공식 `SENS_ATMOS`(8009, ASLUAV dialect)**를 지원합니다. 온도와 상대습도를 한 항목씩 모으고, 실제 대표 표본·그 표본의 측정시각과 위치·구간 통계·원본을 함께 보냅니다. 수집기가 사용하는 전송 단위는 **온도 K, 상대습도 비율 0..1**이며 패킷마다 단위 문자열을 반복하지 않습니다.

위의 TCP 21000 시험 서버를 실행한 뒤, 수집기 옵션을 다음처럼 바꿉니다.

```powershell
.\build\windows-x64\mavlink_telemetry_data_router_interactive.exe --test-weather
```

```sh
./build/linux-x64/mavlink_telemetry_data_router_interactive --test-weather
```

`--test-weather`는 온습도만 보내는 시험 모드이고, 옵션 없이 실행하는 운용 모드도 온습도를 수집합니다. 시험 서버는 측정시각, 위치, 온도·습도, 표본 수·평균·표본분산을 표시합니다. 주소와 설정 파일은 `--test-heartbeat`와 같은 옵션으로 지정합니다. 원본은 메모리에 유지하며 HTTP 204 확인 뒤 해제합니다.

**기본 SITL에는 가상 온습도 센서가 자동으로 생기지 않습니다.** 센서 하나가 10 Hz로 온습도를 보내는 선택 패치와 적용·빌드·시작 명령은 [PX4 온습도 시험 안내](support/px4/README.md)에 있습니다. PX4가 공식 ASLUAV 메시지를 출력하도록 빌드하며 `common.xml`에 사설 필드를 추가하지 않습니다. 현재 WSL 환경에서는 서버와 Linux 수집기를 같은 WSL에서 실행하고, 기존 UDP 14540으로 보낸 자료를 받습니다.

SENS_ATMOS의 wire 단위는 **°C와 %**입니다. 예를 들어 `23.45`, `65.20`을 그대로 보내고 수집기가 `296.60 K`, `0.6520`으로 변환합니다. **습도에 100을 곱해서 보내지 않습니다.** 부팅 기준 µs 측정시각은 실제 앞뒤 항법 위치와 연결할 수 있지만, 이 메시지는 센서 ID·시각 오차·측정 불확도를 제공하지 않습니다. 화면의 센서 `0/0`은 이 소스의 논리적인 단일 관측을 구분하는 값이며 실제 센서 ID나 칩 모델을 알아냈다는 뜻이 아닙니다. 알려지지 않은 오차는 `null`로 보존합니다.

공통 규격 **`HYGROMETER_SENSOR`(12920)**도 기본 온습도 값과 센서 instance를 계속 지원합니다. 다만 이 메시지는 측정시각이 없으므로 측정시각과 그 시각의 위치를 `null`로 보냅니다. 과거 시험에서 추가했던 사설 시각 확장은 더 이상 해석하지 않습니다. 원래 온습도 값만 읽고 뒤의 알 수 없는 확장 바이트는 무시합니다. SENS_ATMOS와 HYGROMETER_SENSOR는 `message_id`가 다른 그룹으로 유지하므로 서로 통계를 합치지 않습니다. 동일한 물리 관측을 두 방식으로 보냈는지는 알 수 없으므로 시험에서는 온습도 출력 경로 하나만 활성화합니다.

**10 Hz 송신은 센서가 대기의 변화를10 Hz로 따라간다는 뜻이 아닙니다.** timestamp는 센서 드라이버가 보고한 측정·읽기 시각이며 센서 응답 지연·외함·복사열 영향까지 제거한 대기의 순간 시각은 아닙니다. 선택한 SHTC3도 제조사 조건(25°C·기류1 m/s)에서 공칭 습도 응답 시간이 약8초입니다. 현재 프로그램은 이런 지연의 역보정·기상장 복원을 구현하지 않았으며, 관련 오차를 모르면 미상으로 유지합니다. [SHTC3 데이터시트](https://sensirion.com/media/documents/643F9C8E/63A5A436/Datasheet_SHTC3.pdf)

대표값은 선택한 실제 표본이며, 여러 위치에서 얻은 평균을 그 표본의 위치에서 잰 값으로 표기하지 않습니다. 구간은 수집기 전송 주기의 묶음이고 측정시각 범위는 별도로 보존합니다. 기존 `unit_map`은 일반 키–값 항목의 단위를 표시하는 설정이며 이 온습도 SI 변환을 바꾸지 않습니다.

운용 서버를 직접 만들 때는 출력 종류를 구분하세요. 일반 관측은 기존 `schema:3`, 온습도는 `mavlink_weather_v1`이고, 두 종류가 함께 있으면 `mavlink_router_bundle_v1`의 `records[]`에 종류와 각 원래 payload를 묶어 **하나의 요청으로** 보냅니다. 따라서 혼합 운용 서버는 이 묶음도 처리해야 합니다. TCP 21000 선택 시험 서버는 하트비트·온습도 시험 전용입니다. 서버를 올려 둔 채 일반 혼합 운용 모드로 바꾸는 시험에는 사용하지 않습니다.

## 실제 센서 SHTC3와 PX4 업데이트

물리 센서는 **SHTC3**를 대상으로 별도 PX4 I²C 드라이버를 제공합니다. 기존 SHT3x 드라이버와 다른 프로토콜이며, SHTC3를 SHT31/35처럼 연결하는 방식이 아닙니다. 소스는 `src/px4/shtc3/`, 적용 패치·버전 목록·검사 도구는 [support/px4](support/px4/README.md)에 둡니다. Pixhawk 6C에서 SHTC3 제품 ID·CRC와 실제 USB SENS_ATMOS→Windows 수집기→TCP 21000 C 시험 서버의 온습도 전송을 확인했습니다. 센서 교정 정확도와 고속 이동 중 위치 정확도를 검증한 결과는 아닙니다.

SHTC3 칩은 **3.3 V I²C, 주소0x70**을 사용합니다. 칩 전원 범위는1.62..3.6 V이므로5 V를 직접 공급하지 않습니다. 모듈의 전원회로·풀업·보드별 I²C 버스는 실제 사용하는 제품 자료에 맞춥니다. 본 작업은 소프트웨어 드라이버와 통신 통합이며 PCB 설계를 포함하지 않습니다. [제조사 사양](https://sensirion.com/products/catalog/SHTC3)

PX4를 업데이트할 때는 새 체크아웃에서 오버레이의 검사 모드부터 실행하고, 통과한 뒤 적용·빌드합니다. 도구는 자동 git pull/reset이나 강제 패치를 하지 않으며, 기존 드라이버 파일에 다른 수정이 있으면 덮어쓰지 않습니다. 공식 MAVLink XML 정의는 유지하고 ASLUAV를 포함하는 wrapper만 추가합니다.

현재 실물 Pixhawk 6C의 PX4 1.16.0에는 [SHTC3 실측 전송 구성](support/px4/pixhawk6c-v1.16-weather/README.md)을 사용합니다. I²C2의 센서 하나를 자동 시작하고, 공식 `SENS_ATMOS`에 측정 명령 시각과 온습도를 담아 USB MAVLink 2로 보냅니다. 제품 ID만 확인하는 [앞선 진단판](support/px4/pixhawk6c-v1.16-diagnostic/README.md)은 별도로 보존합니다.

Windows에서 실제 TCP 21000 시험 서버와 수집기 TUI를 다시 실행하려면 프로젝트 폴더의 **서로 다른 두 터미널**을 사용합니다. 첫 터미널:

```powershell
& '.\build\windows-x64\tools\mavlink_telemetry_data_router_test_server.exe'
```

둘째 터미널은 처음 한 번 설정 예제를 복사한 뒤 실행합니다. 기존 설정 파일이 있으면 복사 단계를 생략합니다. TUI의 수정·저장은 이 복사본에 적용됩니다.

```powershell
Copy-Item '.\config\shtc3-physical.ini.example' '.\build\windows-x64\shtc3-live.ini'
& '.\build\windows-x64\mavlink_telemetry_data_router_interactive.exe' --config '.\build\windows-x64\shtc3-live.ini' --test-weather
```

이 시험은 온습도만 1초마다 묶어 보내며 서버가 검증 후 HTTP 204로 확인한 묶음은 메모리에서 해제합니다. 관측값은 파일에 저장하지 않습니다. 위치 기준·유효 항법과 UTC가 없으면 위치와 UTC는 미상으로 표시합니다. GPS fix가 없더라도 유효한 비행제어기 EKF 전역 좌표가 있으면 위치 출처를 `ESTGPS`로 구분합니다. Linux에서는 대응하는 `build/linux-x64/` 실행 파일과 tty 입력을 사용합니다. 일반 운용 서버로 모든 종류의 데이터를 보낼 때는 `--test-weather`를 빼고 그 서버가 지원하는 주소와 스키마를 설정하세요.

## 시리얼·UDP 자동 선택과 QGC

기본 설정은 **UDP `127.0.0.1:14540`을 수신하면서 COM/tty 후보도 확인**합니다. 먼저 CRC가 맞는 MAVLink를 발견한 입력 하나를 유지하고, 다른 입력은 닫습니다. 선택한 입력이 `link_timeout_ms` 동안 조용하면 다시 양쪽을 탐색합니다. 두 입력의 관측을 섞어 수집하지 않습니다.

PX4 SITL은 기본적으로 **QGC에 14550, API/Offboard에 14540**으로 각각 보냅니다. 따라서 수집기는 14540을 받고 QGC는 기존 14550을 사용하면 됩니다. SILS에 별도 14551 출력을 추가하거나 수집기가 QGC로 다시 보낼 필요가 없습니다. 두 출력은 메시지 구성이 다를 수 있으므로 커스텀 관측도 14540으로 출력되는지 확인하세요. [PX4 기본 포트](https://docs.px4.io/main/en/simulation/#default-px4-mavlink-udp-ports)

**현재 WSL SITL의 14540은 WSL 내부 loopback으로 향합니다.** WSL 안에서 Linux 수집기를 실행하면 받을 수 있습니다. Windows 실행 파일로 받으려면 별도 연결 경로가 필요하며, WSL 자동 중계는 아직 제공하지 않습니다. `udp_bind=0.0.0.0`으로 바꾸는 것만으로 WSL loopback 패킷이 Windows에 전달되지는 않습니다.

야외에서는 USB 텔레메트리의 COM/tty를 자동으로 찾습니다. 현재 프로그램은 관측을 HTTP 서버로 전송하며, **시리얼의 원시 MAVLink를 QGC로 전달하는 기능은 아직 없습니다.**

UDP 입력은 수신 전용이며 하트비트·명령·전송 주기 변경 요청을 보내지 않습니다. 이미 점유된 같은 주소/포트에는 연결 실패를 표시하며, 기존 QGC의 14550 수신을 공유하지 않습니다. 별도 분배 경로가 필요한 환경에서는 송신 측·QGC forwarding·MAVLink Router 등에서 수집기 목적지로 전달해야 합니다.

기존 설정 파일의 포트는 자동으로 덮어쓰지 않습니다. 이전 기본값 14551이 저장되어 있다면 TUI에서 `e` → **32번 udp_port** → `14540` 입력 후 Enter → `s`로 저장하고 재시작하세요. 31번은 수신 주소, 33번은 UDP 사용 여부입니다. 화면의 ACTIVE UDP가 실제 사용 중인 값입니다.

UDP만 사용하려면 `serial_port=none`, `auto_scan=0`, `websocket_url=`로 둡니다. `udp_bind`는 송신기 주소가 아니라 수집기 자신의 수신 주소입니다. 다른 컴퓨터에서 보낼 때는 수신 가능한 인터페이스 주소 또는 `0.0.0.0`과 방화벽을 맞춰야 합니다. 멀티캐스트 그룹 가입은 제공하지 않습니다.

## 실행 후 자동으로 하는 일

1. UDP 수신과 직렬 후보 탐색을 함께 진행합니다. 지정 WebSocket이 있으면 직렬 후보와 같은 탐색 경로에서 확인합니다.
2. 먼저 CRC가 맞는 MAVLink를 확인한 채널 하나를 유지합니다. 데이터가 끊기면 다시 탐색합니다.
3. HEARTBEAT에서 시스템·컴포넌트 ID, 기기 종류와 PX4/ArduPilot 등의 정보를 확인해 목록으로 보여 줍니다.
4. 선택한 비행제어기의 시계에 속한 키–값 관측에 위치·속도·부팅 경과시각을 연결합니다.
5. 비어 있지 않은 관측만 원본·통계와 함께 묶어 기본 1초 간격으로 서버에 보냅니다.
6. 서버가 확정 수락한 묶음에 HTTP 204로 응답하면 해당 메모리 공간을 재사용합니다.

관측은 **메모리에만** 유지합니다. 서버에 연결되지 않아도 수신은 계속하지만 메모리가 가득 차면 새 묶음을 버리고 손실을 표시합니다. 종료 시 남은 미확인 자료는 소실됩니다.

하트비트·항법만 있고 수집할 관측값이 없으면 서버에 빈 요청을 보내지 않습니다. 요청이 느리거나 실패하면 재시도하므로 정확한 정시 도착이나 무조건 초당 한 번의 성공을 보장하지는 않습니다.

## 어떤 데이터를 수집하나요?

기본 키–값 운반 형식은 MAVLink `NAMED_VALUE_FLOAT`와 `NAMED_VALUE_INT`입니다. **온도·습도·미세먼지 같은 항목을 코드에 고정하지 않고 새 키를 자동 등록**합니다. 이름이 추가됐다는 이유로 프로그램을 다시 만들 필요가 없습니다. UDP는 처음 도착한 비어 있지 않은 패킷의 송신 IP/포트를 임시 선택합니다. 유효 MAVLink를 확인하지 못하면 `probe_ms` 후 다른 송신자를 받을 수 있게 초기화합니다. 유효 입력을 선택한 뒤에는 해당 송신자만 유지하며 재연결하면 다시 선택합니다. 다른 송신자의 바이트를 섞지 않습니다. 여러 기체·송신자를 동시에 수집하는 기능은 아닙니다.

현재 한 소스 세션의 키 한도는 16개이며 초과는 거부 수로 표시합니다.

이 메시지에는 단위가 없으므로 `unit_map`에서 지정합니다. 미지정 단위는 `unknown`으로 보존합니다. 기본 압력·온도 메시지와 유효 필드가 표시된 HIGHRES_IMU도 수집합니다. 표준 온습도 메시지는 위의 별도 SI 관측 구조로 수집합니다.

MAVLink 패킷 CRC·하트비트는 직접 검사합니다. 알 수 없는 메시지는 미해독 수로 표시하고 다른 메시지 수신을 계속합니다. XML이 다른 임의의 새로운 메시지를 내용 정의 없이 추측해 해독하지는 않습니다.

다른 컴포넌트의 경과시각을 비행제어기 시각과 임의로 섞지 않습니다. 기기 목록에는 표시하되, 시계 관계가 확인되지 않은 관측은 거부 사유를 표시합니다.

## 위치·시각과 통계를 어떻게 보존하나요?

원본에는 키·단위·값과 그 관측의 부팅 시각·위치·속도·품질이 남습니다. 같은 부팅 시계의 유효 항법 자료로 측정시각에 위치를 연결하고, GPS 보조 좌표와 추정 좌표를 구분합니다. 기존 키–값 경로에서 위치를 연결할 수 없는 관측은 보류한 뒤 사유와 함께 누락 수로 표시합니다. 온습도 경로는 위치를 연결할 수 없어도 값·측정시각·통계를 보존하고 위치를 `null`로 보냅니다.

첫 전역 항법 위치를 자동 원점으로 사용할 수 있습니다. 실제 MAVLink 항법의 고도 기준은 MSL이며 타원체고로 바꾸어 표기하지 않습니다. 위치 오차를 모르면 JSON `null`로 보냅니다.

### GPS와 ESTGPS

`GLOBAL_POSITION_INT`는 비행제어기가 계산한 전역 항법 좌표입니다. GPS 원시 측정값으로 단정하지 않고 다음처럼 표시합니다.

| 화면 | 의미 |
|---|---|
| `GPS` | 최근 GPS fix·센서 시각과 비행제어기 절대 위치 상태가 유효한 좌표 |
| `ESTGPS` | GPS 확인이 없는 유효한 비행제어기 EKF 좌표, 또는 아래 조건을 만족한 수집기 속도 적분 좌표 |
| `NO_ANCHOR` | 사용할 전역 위치가 없고 이전의 유효 GPS 기준점도 없음 |
| `UNKNOWN` | 상태·속도·시각이 유효하지 않거나 허용 기간이 지나 현재 위치를 사용할 수 없음 |

먼저 비행제어기의 유효한 전역 좌표를 사용합니다. 그 출력이 끊긴 경우에만 마지막 유효 GPS 기준점과 `LOCAL_POSITION_NED`의 지상 N/E/D 속도로 짧게 이어 계산합니다. heading/yaw와 대기속도를 곱해 지상 이동으로 사용하지 않습니다. 속도 간격이 500 ms를 넘거나 상태가 오래되거나 잘못되면 적분을 중단합니다. **수집기가 직접 계산하는 구간만 마지막 GPS 기준점부터 최대 30초**이며, 계속 유효하게 들어오는 비행제어기 EKF 전역 좌표에는 이 30초 제한을 적용하지 않습니다.

위치 기준이 없는 실내에서는 임의의 초기 좌표를 만들지 않습니다. 현재 실물은 GPS fix 0이고 전역 위치·GPS 기준점이 없어 `NO_ANCHOR`가 정상이며, 온습도 수집은 계속됩니다. 온습도 JSON에는 `position_source`와 `estimation_age_us`를 보존하고 추정 위치는 품질 비트로도 표시합니다. 알 수 없는 좌표·경과·오차는 `null`/`unknown`입니다. 자세한 유효 조건은 [기술 참조의 위치 정책](docs/REFERENCE.md)를 따릅니다.


`SYSTEM_TIME`의 원시 boot/UTC 대응점도 보존합니다. 그 오차를 알 수 없는 기본 상태에서는 관측별 UTC 추정은 미상으로 남깁니다. `system_time_uncert_us`에 검증한 오차를 지정하면 UTC 추정과 불확실도를 함께 계산합니다.

1초마다 보내는 묶음과 측정시각의 1초 구간은 다릅니다. 같은 측정 구간이 여러 전송 묶음으로 나뉠 수 있어도, 원본 시각은 바꾸지 않습니다. 서버는 `schema:3`의 **`(stream, session, batch_id)`로 중복을 제거**하고 같은 구간의 자료를 합쳐야 합니다. 통계만으로 이동 경로의 원본을 완전히 복원할 수 없으므로 기본값은 원본 포함입니다.

## 화면과 종료

상단에는 온도 K/°C와 상대습도 비율/%, 수신률, 센서별 최근 수신 경과, 측정 boot 시각을 고정 표시합니다. `d`/`k`로 목록을 바꾸어도 이 값은 보입니다. 위치 줄에는 `GPS`·`ESTGPS`·`NO_ANCHOR`·`UNKNOWN`, 마지막 GPS 경과와 오차를 표시하며, 오래된 온습도 값에는 `STALE`을 붙입니다. 서버 요청·204 ACK·보류 큐·프레임/CRC·기기/연결 상태도 함께 확인할 수 있습니다.

- `d`: 하트비트로 발견한 장치 목록
- `k`: 관측값 목록
- 위/아래: 목록 이동
- `e`: 설정 편집, Enter: 검증, `s`: 저장
- Ctrl+C: 수신 종료 후 대기 자료 전송을 시도하고 종료

터미널의 `source status=LOCKED`는 **초록색**으로 표시합니다. 단순히 포트를 열었거나 하트비트 식별을 기다리는 상태는 노란색입니다. 출력이 파일·파이프로 연결되면 색상 제어 문자를 넣지 않습니다.

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
| `test_weather` | 온습도 센서 분리·SI 통계·대표 표본·미상 시각 보존·메모리 상한 |
| `test_time_sync` | UTC 추정·드리프트·극단 시각 |
| `test_config` | 설정 검증·저장·한글 경로·비밀정보 표시 방지 |
| `test_mavlink_input` | 실제 바이너리 패킷 CRC·장치 식별·위치 연결·재부팅 |
| `test_input_transport` | UDP·WebSocket·분할 프레임·ping/pong·가상 tty |

원인을 구분하기 위해 나눈 검사들입니다. **기본 빌드는 검사 OFF**이며, 선택하면 `tests/` 하위 폴더에 만듭니다. CTest는 위 검사와 실제 MAVLink→HTTP 통합 시험, 입력 자동 선택·전환 시험을 실행합니다. Linux에서는 가상 터미널로 연결 상태 색상도 검사합니다.

```powershell
cmake --preset windows-x64-tests
cmake --build --preset windows-x64-tests
ctest --preset windows-x64-tests
```

Linux에서는 `windows`를 `linux`로 바꿉니다. 일반 프리셋으로 다시 구성하면 검사는 OFF가 되지만 이미 생성된 `tests/` 파일은 자동 삭제하지 않습니다.

## 상세 자료와 검증 범위

- [기술 참조](docs/REFERENCE.md): 전체 설정·좌표/시각·출력 규약·교차 빌드·CSV 재생
- [검토·검증 기록](docs/REVIEW.md): 실제 시험과 계산량·메모리·한계

Windows와 Linux x64 실행 파일로 UDP·WebSocket→HTTP 확인 검사를 통과했습니다. PX4 SITL의 별도 UDP 14540을 같은 WSL의 Linux 수집기로 받아 관측 전송과 TCP 21000 시험 모드를 확인했고, 해당 시험에서 QGC의 14550은 유지했습니다. 이후 실물 Pixhawk 6C의 COM5와 SHTC3→Windows 수집기→실제 C 시험 서버의 JSON/TUI 경로도 확인했습니다. Linux 실행 환경은 Ubuntu 24.04 WSL이며, Linux 실물 tty·Ubuntu26.04·회사 서버 연결과 실외 ESTGPS 위치 정확도는 검증하지 않았습니다.
