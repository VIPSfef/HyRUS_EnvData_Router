# 기술 참조 — mavlink_telemetry_data_router 0.11.0

0.11.0은 포트 자동 탐색, QGC 시리얼 중계, 기체·센서 상태 표시, 선택적인 MAVLink 주기 제어, UDP 즉시 송신, 용량 제한 TCP 메모리 대기열, 직접 토큰 입력과 설정 재시작을 제공한다. 0.10.0의 기본 10m 수평 경로 구간·적응 폭·누적 revision과 0.9.0의 위치 출처·조건부 ESTGPS 정책을 유지한다. 네 실행 파일 이름과 공식 SENS_ATMOS wire 규격은 유지하며 이 호스트 변경을 위한 Pixhawk 펌웨어 수정은 없다. 처음 실행할 때는 [README](../README.md), 배포 변경점은 [0.11.0 릴리스 노트](releases/v0.11.0.md)를 읽는다. 이 문서의 명령은 저장소 루트 기준이다. 기본 입력은 바이너리 MAVLink이고, CSV는 명시적인 개발용 재생 모드다.

## 자동 입력과 채널 선택

이 문서의 구현 범위는 `HyRUS_EnvData_Router`의 호스트 처리다. I²C·PX4 uORB·MAVLink 캐리어 발행은 `HyRUS_EnvData_Px4`에서 관리하며, 두 구현 사이의 필드·단위·시각은 [입력 규약](PX4_MAVLINK_CONTRACT.md)을 따른다. 선택 시험 서버는 호스트 송신을 검증하는 도구다.

### 화면 갱신과 기체 생존 판정

입력 흐름은 바이트 수신 → 프레임 CRC 검증 → sysid/compid별 수신 시각 기록 → 시퀀스 중복·누락 집계 → 메시지 해독 → 입력 상태 발행 → 화면 스냅샷이다. 하트비트는 중복 시퀀스여도 실제 수신 시각과 장치 정보를 갱신한다. 관측값의 중복 처리는 그대로 유지한다.

채널 선택과 기체 생존 판정은 별개다. 유효 프레임이 끊기면 `link_timeout_ms` 후 연결을 해제하고 재탐색한다. 프레임은 계속 오지만 선택 기체의 하트비트가 오래되면 채널을 유지하면서 `HEARTBEAT_LOST`로 표시한다. 첫 하트비트를 기다리는 상태는 `WAIT_HEARTBEAT`이고 최근 하트비트가 있어야 `LOCKED`다. 해당 기체의 하트비트 이외 데이터 시각을 별도로 유지하므로 `heartbeat=LIVE`와 `data=STALE`을 구분한다. 무선 모듈과 GCS의 시각은 기체 시각을 갱신하지 않는다.

표시 흐름은 스냅샷 → 터미널 여부 분기다. 파일·파이프 출력에는 ANSI 없는 전체 스냅샷을 기록한다. 터미널에는 현재 크기에 맞춘 화면을 고정 메모리 버퍼에 만들고, 이전 화면과 줄별로 비교하여 변경된 줄과 삭제된 줄만 한 번의 출력으로 반영한다. 새 텍스트를 덮어쓴 다음 줄 끝 잔여 문자를 지워 빈 화면이 먼저 보이지 않게 한다. 화면 크기가 바뀌면 캐시를 무효화하고, 설정 편집도 같은 경로를 사용한다. 화면 갱신에는 전체 지우기나 마지막 행 개행을 사용하지 않는다. 프레임 2개와 갱신 버퍼는 고정 저장소이며 `--sizes`의 `monitor_storage`와 `static_total`에 포함한다.

`packets`는 선택 sysid/compid의 현재 연결 세션 통계다. `rx`에는 중복을 포함하고 `gap~`는 전진한 8비트 시퀀스 차이에서 추정한 누락 수다. `loss~ = 100 * gap / (rx - dup + gap)`이며 분모가 없으면 unknown이다. 반대 순서 도착, 재부팅, 선택적 중계, 256개 이상의 연속 손실은 실제 손실량과 다를 수 있다. CRC 실패는 해당 패킷의 발신자 식별을 신뢰할 수 없어 채널의 `crc_bad`로 별도 집계한다.

기본 `input_mode=mavlink`에서는 stdin을 읽지 않는다. 고정된 두 탐색 경로를 사용한다. 한쪽은 활성 UDP를 수신하고, 다른 쪽은 현재 COM/tty를 통신 속도별로 순회하며 첫 속도 탐색에서 지정 WebSocket도 확인한다. 먼저 CRC가 맞는 프레임을 확인한 경로를 선택하고 나머지는 닫는다. 수신 시각이 같은 두 입력의 물리적 도착 순서를 보장하지 않으며, 같은 반복에서 준비된 경우 UDP를 먼저 확인한다. 한 번 선택하면 해당 소스 하나만 처리하고 중복 수집하지 않는다. 전체 선택 흐름도는 [README의 자동 선택](../README.md#시리얼udp-자동-선택과-qgc)을 참고한다.

각 경로는 MAVLink 파서·boot 시계·항법 이력을 독립적으로 가진다. 선택 전 자료를 다른 경로와 합치지 않고, 선택 이벤트 앞에 소스 세션 reset을 보낸다. 기존 링크가 끊기면 이전 보류 관측을 먼저 마감한 뒤 탐색 상태를 초기화하므로 새 입력의 위치·시각으로 이전 관측을 다시 해석하지 않는다.

UDP는 기본 `127.0.0.1:14540`의 독점 IPv4 수신 소켓이다. Windows에서는 bind 전에 SO_EXCLUSIVEADDRUSE를 설정하고 Linux에서도 SO_REUSEADDR/PORT를 켜지 않는다. 기존 QGC 등이 사용하는 동일 주소/포트에는 bind 실패를 표시한다. 0길이 데이터그램은 무시하고 첫 비어 있지 않은 송신 IP/포트를 임시 선택한다. `probe_ms` 안에 유효 MAVLink가 없으면 소켓은 계속 연 채 임시 peer·부분 데이터그램·파서를 초기화해 다른 송신자를 받을 수 있게 한다. CRC가 맞는 프레임으로 입력이 선택된 뒤에는 연결 동안 같은 peer만 받는다. 단순 UDP 패킷만으로 초록색 연결을 표시하지 않는다. 최대65,507B 데이터그램 전체를 고정 버퍼에 받아 작은 읽기로 나눠 전달하므로4096B 입력 버퍼 때문에 뒷부분을 자르지 않는다. UDP에는 발신이 없으며 it_write도0을 반환한다.

자동 탐색의 read 대기는 경로당 최대20ms, UDP receive 시도는 호출당최대256회다. 파싱·포트 열기·OS 스케줄링을 포함한 하드 상한은 아니다. 특히 선택한 WebSocket의 연결은 아직 동기식이므로 `connect_timeout_ms` 동안 UDP 처리도 지연될 수 있다. 기본값처럼 WebSocket을 지정하지 않으면 이 연결 대기는 없다. UDP 손실/역전/커널 큐 포화는 생길 수 있고 이후 HTTP 확인은 수집기 이전 손실을 복구하지 않는다. IPv4 유니캐스트 및 수신 인터페이스에 도달하는 브로드캐스트를 다루지만 멀티캐스트 그룹 가입·IPv6·동시 다중 peer는 제공하지 않는다.

Windows는 QueryDosDevice로 현재 존재하는 COM 후보를 찾고, Linux는 `/dev/serial/by-id/*`, `/dev/ttyACM*`, `/dev/ttyUSB*`를 탐색해 장치 중복을 제거한다. 검색 시 포트를 열지 않으며 실제 확인 순서에 도달한 포트만 연다. 기본 `serial_port=auto`, `auto_scan=1`의 탐색 순서는 다음과 같다.

1. `serial_baud`를 우선 속도로 정한다. 기본값은 115200이며 이 속도로 현재 전체 COM/tty를 먼저 검사한다. 명시한 포트가 있으면 우선 확인하고, 같은 장치를 자동 후보로 중복 검사하지 않는다.
2. 전체 포트에서 유효 MAVLink가 없으면 `115200, 57600, 230400, 460800, 921600, 38400, 19200, 9600`에서 우선 속도를 제외한 순서로 다음 속도를 선택한다. 한 포트에서 모든 속도를 소진한 뒤 다른 포트로 넘어가는 방식이 아니라, 한 속도로 전체 포트를 마친 뒤 다음 속도로 진행한다.
3. 각 속도 탐색을 시작할 때 포트 목록을 새로 열거한다. 지정 WebSocket은 우선 속도의 첫 탐색에만 포함한다. 마지막 속도까지 찾지 못하면 우선 속도부터 새 탐색을 시작한다.
4. CRC가 맞는 MAVLink를 확인하면 선택한 포트와 실제 속도를 유지한다. `link_timeout_ms` 동안 유효 프레임이 없으면 이전 선택을 해제하고 우선 속도부터 다시 탐색한다. UDP 경로는 후보 탐색과 병행한다.

`auto_scan=0`은 자동 포트 열거와 다른 baud 시도를 모두 끈다. 명시한 `serial_port`를 `serial_baud` 하나로 확인하며, 설정 WebSocket과 활성 UDP는 계속 사용할 수 있다. `serial_port=auto` 또는 `none`에는 명시한 포트가 없으므로 이 모드에서 직렬 후보가 생기지 않는다. `none`만으로 자동 검색을 끄지는 않는다.

TUI `source` 줄의 `baud`는 현재 검사하거나 선택한 실제 속도다. 설정 화면의 `serial_baud`는 탐색 우선값이며 선택 결과와 다를 수 있고, 자동 탐색이 이 설정을 덮어쓰지 않는다. UDP와 WebSocket에는 직렬 baud를 적용하지 않는다.

WebSocket은 클라이언트로 접속하며 바이너리 데이터만 MAVLink 바이트 스트림으로 처리한다. ws/wss, 분할 프레임, ping/pong, 부분 TCP 읽기와 TLS 인증서 검증을 지원한다. 텍스트/JSON WebSocket은 별도 변환 없이는 입력하지 않는다.

MAVLink CRC가 맞는 프레임을 받으면 해당 채널을 유지한다. 직렬/WebSocket 후보에서 `probe_ms` 안에 없으면 다음 후보로, 선택한 입력에서 `link_timeout_ms` 동안 유효 프레임이 없으면 우선 baud부터 다시 양쪽을 탐색한다. 포트를 열지 못한 경우에도 다음 후보로 진행한다. 등록한 채널 밖의 임의 네트워크 주소를 검색하지 않는다.

기기는 HEARTBEAT의 system/component ID, MAV_TYPE, MAV_AUTOPILOT로 식별한다. component ID 숫자만으로 종류를 추측하지 않는다. 종류를 모르는 기기는 미상으로 표시한다. `target_system=0`, `target_component=0`은 첫 비행제어기를 선택한다. 다른 기기들은 목록에 남지만 시계가 다른 관측을 자동 합치지는 않는다.

UDP를 제외한 설정 포트와 WebSocket에는 연결 후, 자동 검색 포트에는 유효 MAVLink 확인 후 1Hz로 중계기 HEARTBEAT를 알린다. source system255/component191, type ONBOARD_CONTROLLER/autopilot INVALID를 사용한다. `mavlink_rates_enabled=1`이면 선택한 시리얼 기체에 아래의 메시지 주기 요청도 보낸다. 비행·arming·임무 명령이나 영구 파라미터 변경은 하지 않는다.

### 대역폭 기반 메시지 주기 제어

기본값0에서는 주기를 요청하지 않는다. 활성화하면 CRC가 검증되고 선택 비행제어기의 하트비트가 유효한 시리얼 링크에만 `MAV_CMD_SET_MESSAGE_INTERVAL`을 MAVLink2 COMMAND_LONG으로 보낸다. UDP·WebSocket·탐색 중인 포트·GCS에는 요청하지 않는다. PX4의 하트비트 주기는 변경하지 않고1Hz 예산을 보존한다. 요청은 현재 MAVLink 인스턴스의 스트림에 적용되며 센서 하드웨어 샘플링률이나 저장된 FC 파라미터를 바꾸지 않는다. [MAVLink 주기 명령](https://mavlink.io/en/messages/common.html#MAV_CMD_SET_MESSAGE_INTERVAL), [PX4 v1.16 처리 코드](https://raw.githubusercontent.com/PX4/PX4-Autopilot/v1.16.0/src/modules/mavlink/mavlink_receiver.cpp).

3초간 실제 받은 바이트를 측정한 뒤 초기 예산을 실측량의125%와 제어 예약량의 합으로 정한다. UART는8N1 기준 baud/10B/s의70%를 상한으로 사용할 뿐 무선 실효 대역폭으로 간주하지 않는다. 제어 예약은 명령·응답의 최대 wire 크기와 최대 명령 빈도로 계산한다. 이미 들어오는 비관리·비활성 트래픽은 실제 wire 바이트 평균으로 예약하며, 자체 주기 명령의 ACK를 제어 예약과 중복으로 더하지 않는다. 요청 증가분에만 최대 메시지 크기와 서명 여유를 적용한다.

부차 스트림의 최소 복구 주기를 남긴 뒤 핵심 스트림의 실효1Hz에 필요한 요청을 먼저 배분한다. 남는 예산으로 부차1Hz 요청, 핵심 증속, 자세 증속 순으로 배분하며, 물리 예산이 그 기본량에도 부족할 때에만 핵심 주기를 낮춘다. 같은 비용 함수와 단조 이분 탐색으로 이 순서를 계산한다. 관측 평균을 완화하고 예산을 넘지 않는10% 이내 주기 변동은 유지하여 작은 표본 개수 변화로 명령을 반복하지 않는다. 혼잡 감속과 핵심 기본 주기 복구는 이 변경 억제의 예외다.

수락된 주기와 실제 수신률이 다른 공통 감속을 비용 계산에 반영한다. 명령 적용 뒤5초를 제외하고15초간 관측한3개 이상의 메시지가25% 범위에서 비슷한 수신/요청 비율을 보여야 한다. 핵심 메시지2개 이상과 항법·센서 또는 항법·자세의 서로 다른 역할이 필요하며, 느린 센서 하나만으로 보상하지 않는다. 가장 높은 비율을 보수적으로 올림하여0.25~1.0 범위에서 적용하고, 학습된 값은 다음 명령 때 초기화하지 않는다. 핵심 실효 기본량은 이 비율로 보상하되 아래 요청 상한은 그대로 지킨다. 패킷 손실·라디오 혼잡·명령 실패 중에는 더 낮은 비율을 학습하지 않는다. 비용 모델이 바뀌면 시험 증액 비교의 기준 관측도 새로 시작한다.

| 분류 | 요청 상한 |
|---|---|
| GPS_RAW_INT / GLOBAL_POSITION_INT | 10 / 20Hz |
| HYGROMETER_SENSOR 또는 관측된 SENS_ATMOS 한 경로 | 10Hz |
| VFR_HUD 또는 관측된 AIRSPEED 한 경로 | 10Hz |
| ESTIMATOR_STATUS | 5Hz |
| 관측된 ATTITUDE / ATTITUDE_QUATERNION | 각2Hz 이하 |
| 그 밖의 허용 목록에 있는 주기 텔레메트리 | 현재1Hz보다 빠른 경우만1Hz 이하로 제한 |

상한은 목표 보장치가 아니다. 실제 센서 발행률·PX4 자체 대역 제한·무선 손실 때문에 수신률은 낮을 수 있다. 명령·파라미터·임무·이벤트·미등록 메시지를 임의로1Hz 스트림으로 만들지 않으며, 이미 느린 부차 메시지를 늘리지 않는다. 먼저 고속 부차 스트림을 낮추고 주요 스트림을 요청한다. 예산이 부족하면 주기를 함께 늘리고, 심한 혼잡에서도 요청값0의 의미인 '기본값 복원'을 감속으로 오용하지 않는다.

5초 창에서 선택 기체의 추정 누락률이5%를 넘거나 최근 RADIO_STATUS의 여유 txbuf가25% 미만이면 예산을20% 낮춘다. 정상 ACK와20초 기준 관측이 확보되면 시험 증액한다. 예산 이용률65% 이상은10%, 그 미만도5%를 시험하여 짧은 패킷과 보수적인 크기 예산의 차이 때문에 증속이 막히지 않게 한다. 최근 RADIO_STATUS가 있으면 여유 txbuf50% 이상을 추가로 요구하고 UART 상한은 항상 지킨다.

시험 증액 후5초 안정화와20초 평균 관측으로 총수신 B/s 또는 핵심 메시지 수신률이2% 이상 늘었는지 확인한다. 효과가 없으면 이전 예산으로 돌아가 HOLD하며 새 센서 메시지 또는 실측량20% 증가가 있을 때 탐색을 재개한다. 혼잡·명령 실패·하트비트 상실·QGC 주기 변경은 시험을 취소한다. 요청 주기는10ms 단위로 느린 쪽에 맞춘다. 화면 `rates`는 PROBING/HOLD 상태, 실측 B/s·예산 B/s·공통 비율 추정 `scale~`·ACK/거부/무응답 횟수를 표시한다. `scale~100%`는 충분한 관측 전의 기본 가정이며, 이 값과 요청 수락은 실제 수신률 달성 보장이 아니다.

한 번에 명령 하나만 대기하고 전체 프레임 쓰기 후2초간 ACK를 기다린다. 재시도는1회이며 다음 명령 간격은 최소500ms다. 거부·지원되지 않음·최종 무응답은 같은 연결에서 차단한다. 재시도 후 늦은 ACK는 메시지 ID를 식별할 수 없어2초 격리하며, 목적지가 다른 ACK와 검증할 수 없는 서명 ACK는 수락하지 않는다. QGC가 주기 변경을 요청하면 현재 귀속을 취소하고5초간 양보한다. 설정을0으로 바꾸면 추가 주기 요청을 중지하며, FC에 이미 적용된 주기를 자동 복원하는 동작은 아니다.

## 설정 전체

QGC 경로는 선택한 시리얼에만 붙는 별도 양방향 UDP 소켓이다. 기본 목적지는 `127.0.0.1:14550`이며 로컬 포트는 OS가 고른다. connected UDP로 목적지와 동일한 IP·포트의 응답만 받고, 한 순회에서 최대 8개 데이터그램을 읽어 32개 완전한 프레임 큐에 넣는다. 시리얼 송신은 기존 하트비트와 QGC 요청이 공유하며 부분 쓰기가 끝난 뒤 다음 프레임을 고른다. 큐 포화·잘못된 CRC·소켓 오류와 연결 종료 시 남은 요청은 QGC 드롭/오류 진단으로 남는다. UDP·WebSocket 입력은 이 소켓으로 중계하지 않는다.

공식 c_library_v2 커밋 `6b4243f0e54b42ddb1e65df7a3a5e8c99d15ec1f`의 parser와 all.xml CRC 목록을 사용한다. `support/update_mavlink.py`가 지정 Git checkout에서 변경 없는 protocol 헤더와 CRC 목록을 가져온다. 전달은 수신 길이·checksum·signature를 유지하며 다시 서명하거나 시퀀스를 만들지 않는다. 패딩을 제거하는 재직렬화 helper를 쓰지 않는다. 서명 프레임은 CRC와 구조만 확인하여 전달하고, 기존 센서 디코더의 미검증 서명 거부 정책은 유지한다. 따라서 서명 스트림의 QGC 중계와 센서 관측 수용 상태는 다를 수 있다.

설정은 UTF-8 `key=value`이며 알 수 없는 키·중복·잘린 행·범위 밖 값은 시작 전에 거부한다. Windows 한글 경로를 지원한다. TUI의 `s`는 원자적 저장만 하고, `r`은 저장 후 같은 프로세스 안에서 수신기를 재시작한다. 항목 입력 중의 `r`은 입력 문자이며 Enter로 검증한 뒤에 재시작 단축키를 사용할 수 있다.

적용 흐름은 초안 검증·원자적 저장 → 디코더와 포트를 유지한 채 수신 일시 정지 → 기존 목적지로 보류 전송 → 디코더의 마지막 관측 처리 및 포트 반환 → 남은 전송과 송신·모니터 작업자 종료 → 새 설정과 실행 식별자로 재초기화 → 입력 재탐색이다. URL이 같으면 보류 전송에 새 인증 설정을 먼저 적용한다. 따라서 `401` 응답으로 큐가 막혀 있어도 토큰을 고쳐 `r`을 누르면 같은 자료를 새 토큰으로 재시도할 수 있다. URL이 다르면 기존 목적지·인증 설정으로 보류 데이터를 전송한다. 수신기 두 개가 포트를 동시에 열지 않으며 ACK 전의 큐를 새 URL·원점·세션으로 재해석하지 않는다. 재시작 전송이 연속 3회 실패하면 새 설정 적용을 취소하고 기존 설정을 유지한다. 큐가 가득 차 미완성 구간이 남으면 수신을 정지한 채 전송을 재시도하며, 그 구간을 큐에 보존한 뒤 기존 입력을 재개한다. 저장한 초안은 남으므로 서버 복구 후 `r`로 다시 적용한다. 적용 완료는 `config restarts`에 표시하며 `run_elapsed`는 프로세스 시작부터 계속 누적한다. 누적 손실 진단은 설정 적용 뒤에도 보존한다. Ctrl+C 종료 요청이 재시작보다 우선한다. 현재 또는 새 입력 모드가 CSV이면 앱 내부 재시작을 거부한다.

| 항목 | 기본 / 범위 |
|---|---|
| url | HTTP/HTTPS 주소 또는 JSON 미리보기 `-` |
| stream, session | ASCII 영숫자·`_-.`, 1–64자 |
| token_env | `TB_TOKEN`, 직접 입력한 token이 비어 있을 때 읽을 인증 환경변수 이름 |
| token | 기본 빈 값. 직접 입력한 HTTP Bearer 토큰이 환경변수보다 우선, 최대1024바이트·CR/LF 금지; TUI47번 |
| origin_lat/lon/alt | 고정 원점. 실제 MAVLink에서는 위·경도 deg, MSL m |
| origin_auto | 1: 첫 유효 GLOBAL_POSITION_INT를 원점으로 고정, 0: 지정값 |
| input_mode | mavlink 또는 csv |
| serial_port | 기본 auto. none은 우선 포트 없음. COM 이름 또는 tty 경로는 우선 포트 지정 |
| serial_baud | 기본 115200, 자동 탐색의 우선 속도. 지원: 9600/19200/38400/57600/115200/230400/460800/921600 |
| udp_enabled | 1, UDP 입력 사용0/1 |
| udp_bind | 127.0.0.1, 로컬 IPv4 literal. 0.0.0.0은 모든 IPv4 인터페이스 |
| udp_port | 14540, 1–65535. PX4 SITL API/Offboard 기본 목적지; QGC14550과 분리 |
| websocket_url | 빈 값 또는 ws/wss 주소, 최대2047자 |
| auto_scan | 기본 1: 전체 직렬 포트를 속도별로 검색. 0: 지정 포트·지정 속도만 사용 |
| probe_ms | 2000, 100–60000 |
| link_timeout_ms | 5000, 100–3600000 |
| reconnect_ms | 1000, 100–60000 |
| target_system/component | 0 자동, 1–255 지정 |
| system_time_uncert_us | 4294967295 미상. 검증한 source SYSTEM_TIME 오차를 µs로 지정 |
| accel_bound_mps2 | 10, 0–1e6. 위치 보간용 가속도 가정 |
| duration_ms | 0 무기한, 양수는 입력 수집 제한 시간 |
| unit_map | 빈 값 또는 `temp:C;humidity:%;pm25:ug/m3` |
| send_interval_ms | 1000, 1000–60000. 측정 대기열의 전송 라운드를 시작하는 주기 |
| http_min_interval_ms | 500, 500–60000ms. 직전 HTTP 완료 후 다음 요청까지 최소 대기; 모든 측정·상태·재시도·종료 전송 공유; TUI48번 |
| status_interval_ms | 0 끔, 활성 범위1000–60000ms. 10000이면 새 관측 없이도10초마다 별도 상태 보고; TUI41번 |
| mavlink_rates_enabled | 0 끔, 1은 선택 시리얼 링크의 우선순위·대역폭 기반 메시지 주기 요청; TUI42번 |
| udp_backup_enabled | 0 끔, 1이면 독립 UDP 센서·상태 전송; TUI43번 |
| udp_backup_host | auto는 HTTP URL 호스트. IPv4·호스트명·IPv6 명시 가능; TUI44번 |
| udp_backup_port | 0은 HTTP TCP 포트+1, 또는1–65535 명시; TUI45번 |
| tcp_buffer_max_mb | 1024MiB(1GiB). 대기 JSON·노드 관리 정보 합계 상한, 1–65536MiB(32비트는 최대4095MiB); TUI46번 |
| connect_timeout_ms/io_timeout_ms | 각각1000, 100–60000 |
| max_windows | 4, 요청당 최대4묶음 |
| refresh_ms | 1000, 250–10000 |
| idle_warn_ms/ack_warn_ms | 2000/5000, 250–3600000. 화면 경고 기준 |
| summary | 0 원본 포함, 1 기존 key–value 원본 생략; 온습도 원본에는 적용하지 않음 |
| spatial_step_m | 10, 0.01–10000m. 목표 수평 경로 구간 폭; TUI34번 |
| spatial_gap_ms | 2000, 100–60000ms. 공간 경로를 잇는 관측 간격 한도; TUI35번 |
| spatial_enabled | 1, 0/1. 온습도 공간 구간 집계 사용; TUI36번 |
| spatial_adaptive | 1, 0/1. 실제 표본 간격에 따라 다음 구간의 폭 적응; TUI37번 |
| qgc_enabled | 1, 0/1. 선택한 시리얼↔QGC 전달; TUI38번 |
| qgc_host | 127.0.0.1. QGC 목적지 IPv4 주소; TUI39번 |
| qgc_port | 14550, 1–65535. QGC 목적지 UDP 포트; TUI40번 |

URL query는 화면에서 숨기지만 설정 파일에는 원문으로 저장된다. URL userinfo와 fragment는 거부한다. Windows timeout은 WinHTTP 단계별이며 Linux 총 HTTP timeout은 connect+2×io로 적용한다. DNS/TLS/OS까지 포함한 하드 실시간 상한을 뜻하지 않는다.

직접 입력은 `e` → `47` → Enter → Ctrl+U → 토큰 붙여넣기 → Enter → `r` 순서다. Ctrl+U로 기존 입력을 지운 뒤 교체한다. TUI는 입력과 저장된 값을 가리고 로그에도 원문을 남기지 않는다. `token`이 있으면 이를 우선 사용하며, 비우면 `token_env` 환경변수 값을 사용한다. Windows 저장 시에는 현재 Windows 사용자용 DPAPI 암호문(`token=dpapi:...`)을 기록하므로 다른 PC·사용자는 다시 입력한다. Linux에서는 POSIX 파일 권한을 지원하는 파일 시스템의 설정 파일에 원문을 저장하고 권한을 `0600`으로 제한한다. WSL의 metadata 없는 `/mnt/c` 대신 Linux 홈 디렉터리처럼 이 권한이 적용되는 위치를 사용한다. 인증값은 HTTP Bearer 헤더에만 넣으며 UDP에는 넣지 않는다.

Linux에서 직접 토큰을 저장할 때는 내용을 쓰기 전에 임시 파일이 현재 사용자 소유의 일반 파일이고 권한이 정확히 `0600`인지 검사한다. 지원하지 않는 파일 시스템에서는 토큰을 기록하지 않고 임시 파일을 제거하며 기존 설정을 유지한다.

`token_env=TB_TOKEN`은 인증값이 아니라 그 값을 읽을 환경변수 이름이다. 환경변수 방식을 계속 쓰려면 `token`을 비워 둔다. 변수 이름은 영문자 또는 `_`로 시작하는 영문자·숫자·`_` 최대64자이며, 실제 값은 라우터를 실행할 환경의 해당 변수에 둔다. 로컬 시험 서버는 인증을 요구하지 않는다.

## SENS_ATMOS·HYGROMETER_SENSOR와 SI 관측 구조

측정시각이 필요한 단일 온습도 입력에는 공식 ASLUAV dialect의 `SENS_ATMOS`를 사용한다. common.xml 메시지는 아니지만 공개된 공식 MAVLink 정의이며 사설 확장이 아니다. 기본 본문16 B, CRC_EXTRA144, 메시지 ID8009이므로 MAVLink2가 필요하다. unsigned 패킷은 최대28 B(헤더10+본문16+CRC2)이며 끝0바이트는 생략될 수 있다.

| wire offset | 공식 필드 | 형식·단위 |
|---|---|---|
| 0 | timestamp | uint64, 부팅 기준 µs |
| 8 | TempAmbient | float32, °C |
| 12 | Humidity | float32, 상대습도 % |

수집기는 다음 식으로 SI 값에 정규화한다. wire 상대습도65.20은65.20%이며6520이나0.6520을 보내는 규격이 아니다.

```text
temperature_K = TempAmbient + 273.15
humidity_fraction = Humidity * 0.01
variance_SI = scale^2 * variance_source
```

SENS_ATMOS에는 센서 ID·instance·품질·시각 오차·측정분산이 없다. 수집기의 논리 그룹 `sensor_id=0,sensor_instance=0`은 이 source의 단일 온습도 입력을 식별할 뿐 물리 센서 종류를 증명하지 않는다. source system/component/session과 ENU 원점은 바깥 전송 레코드로 구분한다. 측정시각은 같은 PX4 부팅 시계의 검증·분류된 앞뒤 항법 두 점 사이에서 위치를 보간한다. GPS/ESTGPS 분류와 제한은 아래 위치 정책을 따른다. timestamp의 µs 자료형을 근거로 시각 정확도1µs를 가정하지 않는다. 상대 시각 오차와 측정분산은 null이고, 이 때문에 시각 오차까지 합산한 위치 오차 추정도 null일 수 있다. 위치 자체를 보간할 수 없는 경우에도 관측값·시각·통계를 보존하고 위치를 null로 남긴다.

같은 source가 HYGROMETER_SENSOR와 SENS_ATMOS를 함께 보내면 실제 wire의 message_id를 그룹 키에 포함해 서로 분리한다. 단위가 같다는 이유로 평균·원본을 합치지 않는다. 동일 물리 관측의 중복인지 확인할 식별자는 없으므로 시험 패치는 SENS_ATMOS 하나만 선택한다. 표준 메시지만으로 SENS_ATMOS에 없는 센서 ID를 복원하거나 같은 component의 여러 물리 센서를 자동 분리할 수 없다.

공식 common 메시지 `HYGROMETER_SENSOR`(12920, CRC_EXTRA20)도 계속 지원한다. 기본 본문5 B의 wire 순서는 온도 int16(offset0,0.01°C), 습도 uint16(offset2,0.01%), 센서 instance uint8(offset4)이다. 정수 T,H는 `T*0.01+273.15` K, `H*0.0001` 비율로 변환한다. 예를 들어2345,6520은296.60 K와0.6520이다. 이 메시지는 측정시각이 없으므로 t_us/position_m/velocity_mps를 null로 남긴다. 수신시각이나 최근 위치를 측정 메타데이터로 대입하지 않는다. device ID도 미상이므로0이다.

v0.7.0의 HYGROMETER_SENSOR 사설 time/device/uncertainty 확장은 제거했다. 기본 필드 뒤 바이트는 알 수 없는 확장으로 무시하고 시각으로 해석하지 않는다. PX4 시험 패치도 공식 common.xml을 바꾸지 않는다. HYGROMETER_SENSOR의 습도 배율 교정은 해당 송신기의 문제를 해결하는 별도 사항이며, 표준 SENS_ATMOS의 float RH에는100배를 적용하지 않는다.

`weather.h`의 `WEATHER_SI_REGISTRY=1`은 temperature→K, humidity→비율을 정의한다. 그룹 JSON은 단위 문자열을 매 표본마다 반복하지 않는다. 기존 key–value schema3의 unit_map은 단위 표시이며 값의 SI 변환 설정이 아니다. 일반 압력·내부 온도 등 기존 경로의 단위까지 새 구조와 같다고 가정하지 않는다. 물리 센서 드라이버가 내부 uORB에 발행한 값과 그 관측시각을 PX4 MAVLink stream이 공식 메시지로 보내는 구조이며, 가상 센서 예제는 물리 센서 드라이버를 대체하지 않는다.

기존 온습도 원본·전송 시간 요약 JSON은 다음과 같다. `host_window_ms`는 호스트 배치 시간이며 거리 구간 식별자가 아니다. 0.10.0의 공간 구간은 같은 온습도 레코드의 `spatial` 객체에 `mavlink_spatial_v1`으로 전달한다. temperature/humidity 아래 센서 배열을 사용하며 필드 이름을 관측 종류의 별도 키로 등록하지 않는다.

```text
weather: {
  si_registry: 1,
  host_window_ms: "collector monotonic window start",
  n, raw_n, raw_dropped,
  measurements: {
    temperature: [{message_id, sensor_id, sensor_instance, representative, statistics, samples, raw_n, raw_dropped}],
    humidity:    [{message_id, sensor_id, sensor_instance, representative, statistics, samples, raw_n, raw_dropped}]
  }
}
```

`representative`와 `samples[]`는 `t_us`(decimal string/null), `position_m`·`velocity_mps`(ENU/null), `value`(SI), `measurement_variance`(SI²/null), `time_uncertainty_us`, `navigation_uncertainty_m`, `position_uncertainty_m`, `quality`, `seq`, `position_source`, `estimation_age_us`, `velocity_valid`, `navigation_segment`를 갖는다. 속도 배열의 존재와 측정시각의 속도 유효성을 구분한다. 표본의 측정 불확도는 알 수 없으면 null이며 패킷의 해상도를 측정 정확도로 바꾸지 않는다. `position_uncertainty_m`은 알려진 항법 오차와 `|v|·time_uncertainty_us/1e6`의 합인 현재 속도 기반 추정이며 교정된 절대상한이 아니다.

대표 표본은 알려진 측정시각·위치를 우선하고, 위치가 있는 표본끼리는 GPS를 ESTGPS보다 우선한다. 이후 알려진 위치 오차 추정, 그 크기, 알려진 측정분산, 그 크기, 알려진 시각 오차와 그 크기, 결정적인 표본 순서로 선택한다. 원본 표본의 값·시각·위치를 함께 유지하며 통계 평균을 대표 좌표에서의 측정값처럼 사용하지 않는다. quality의 `0x08000000`은 측정시각 미상, `0x04000000`은 측정 위치 미상, `0x10000000`은 ESTGPS 추정 위치다. 기존 미인증 시계 플래그 등 다른 비트도 보존한다. 품질 비트의 숫자 크기를 센서 정확도 순위로 해석하지 않는다.

`position_source`는 `GPS`·`ESTGPS`·`UNKNOWN` 문자열이다. `estimation_age_us`는 마지막 유효 GPS 기준점부터의 boot 시각 경과 µs이며, GPS 표본은 0, 기준점 시각을 모르면 null이다. 유효한 FC EKF 전역 좌표라도 이전 GPS 기준점이 없으면 ESTGPS와 경과 null을 함께 보낼 수 있다. 두 필드는 `representative`와 각 `samples[]`에 추가하며 envelope 이름 `mavlink_weather_v1`은 유지한다. 수신 서버는 추가 필드를 허용하고, 추정 출처·나이를 표본과 함께 보존해야 한다.

기존 시간 요약의 `statistics`는 n·평균·표본분산 `M2/(n−1)`·min/max와 `time_known_n`·측정시각 시작/끝을 갖는다. n<2이면 분산은 null이다. 측정시각 범위는 그 시각을 아는 표본만 사용하며 `time_known_n=0`이면 양 끝도 null이다. 시각 미상 표본도 평균·분산 n에는 포함된다. 이 분산은 이동에 따른 실제 공간 변화와 잡음을 모두 포함하며 단일 센서 측정의 불확도와 다르다.

한 온습도 배치의 원본은 최대128개, 그룹은 최대16개다. 원본 한도 초과 표본도 통계에 반영하고 raw_dropped를 기록한다. 그룹 한도 초과와 잘못된 물리량은 거부 수로 집계한다. 상대습도는 0..1, 온도는 0 K 이상, 유효 값·좌표·속도는 유한해야 한다. 미래 항목은 SI 사전 버전과 식별자를 늘려 정의해야 하며 알 수 없는 단위를 추측하지 않는다.

시험 모드 `--test-weather`는 `mavlink_weather_v1` envelope의 `records[]`에 `{seq,generation,session,source,frame,origin_wgs84,time_basis,host_time_basis,clock_model,weather,spatial?}`를 넣는다. 순번·시각의 64비트 표현 규약은 각 schema를 따른다. 서버는 `(stream, session, seq)`로 중복 제거하고 요청 전체 검사·수락 뒤204로 답해야 한다. 실패·202·응답 단절에서는 동일 순번과 관측을 재전송한다. `--summary`는 기존 key–value 출력에 적용하며 온습도 원본을 생략하지 않는다.

PX4의 기준 커밋·ASLUAV 포함 빌드·가상 센서 명령은 별도 `HyRUS_EnvData_Px4`에서 관리하며, Router와의 연결은 [입력 규약](PX4_MAVLINK_CONTRACT.md)을 따른다. HYGROMETER_SENSOR는 온도와 상대습도를 재는 hygrometer의 센서 인스턴스를 전달하는 메시지다. 비행제어기 내부 barometer/IMU 온도를 주변 대기 온도로 간주하지 않는다. 표준에 다른 센서 메시지도 있지만 공통적인 측정시각·품질·이동 관측 통계를 모두 제공하는 한 가지 범용 기상 메시지가 되는 것은 아니다.

그룹 식별자는 `(metric,message_id,sensor_id,sensor_instance)`이고 message_id는 한 그룹에 한 번 기록한다. 샘플마다 반복하지 않으며 `8009`는SENS_ATMOS, `12920`은HYGROMETER_SENSOR다. API의0은출처메시지미상이고운용MAVLink해독기는실제ID를기록한다.

센서의 동적응답은 시계동기화와 다른 오차다. 드라이버가 보고한 timestamp_sample은 읽기·변환 시각을 나타낼 수 있으며, 그 시각의 대기 물리량을 지연 없이 측정했다는 보장이 아니다. 1차응답 가정 `τ·dy/dt+y=x`에서 센서 지연으로 인한 공간 오차 규모는 대략 `|v|·τ`지만 설치조건에 따라 달라지는 추정이며 위치 보간의 보정값으로 자동 적용하지 않는다. 선택한 SHTC3의 제조사 공칭 습도응답은25°C·기류1 m/s 조건에서약8초이며 실제 외함·통풍·복사열은 별도 검증해야 한다. 현재 코드는 역필터·지연보정·물리 기상장 복원을 구현하지 않는다. [제조사 SHTC3 데이터시트](https://sensirion.com/media/documents/643F9C8E/63A5A436/Datasheet_SHTC3.pdf)

공식 근거: [SENS_ATMOS](https://mavlink.io/en/messages/ASLUAV.html#SENS_ATMOS), [HYGROMETER_SENSOR](https://mavlink.io/en/messages/common.html#HYGROMETER_SENSOR), [MAVLink 직렬화와 CRC](https://mavlink.io/en/guide/serialization.html), [SI](https://www.bipm.org/en/publications/si-brochure).

## 10m 수평 경로 구간 — 0.10.0

공간 구간의 목적은 이동 속도가 달라도 비슷한 이동거리 범위의 관측을 비교하는 것이다. 현재 적용 대상은 지원 온습도 관측이며 일반 NAMED_VALUE/CSV의 기존 통계 규약은 바꾸지 않는다. 기존 `mavlink_weather_v1`의 시간 요약과 `schema:3`의 1초 측정시각 창은 부가 자료로 유지한다. 1초는 기본 **요청 주기**, 10m는 기본 **공간 구간 목표 폭**이다. 한 요청에 여러 공간 구간을 넣으며 수신 원본을 초당 한 표본으로 줄이지 않는다. 온습도 원본은 호스트 배치당 최대128개이며 초과 표본도 기존 시간 통계와 ingest의 공간 통계에는 반영하고 `raw_dropped`로 표시한다. 공간 통계가 있다는 이유로 초과 원본까지 보존된 것으로 해석하지 않는다. 공간 레코드·전송 큐도 고정 상한과 손실 계수를 갖는다.

### 경로와 적응 폭

측정시각의 ENU 수평속력 `u[k]=hypot(v_E[k],v_N[k])`로 `s[k+1]=s[k]+(u[k]+u[k+1])*Δt/2`를 계산한다. 이는 수평 경로 길이이며 시작점에서의 직선 거리·3차원 이동거리·누적 고도가 아니다. 위경도 위치 차분의 길이를 매 표본 더하면 정지 위치 잡음만으로 거리가 늘 수 있어 그 방식을 사용하지 않는다. 속도는 유한하다는 사실 외에 해당 측정시각의 유효성이 필요하다. 속도 자체의 잡음도 속력의 양의 편향을 만들 수 있으므로 정지 경로 거리의 절대 정확도를 보증하지 않는다. [거리 잡음 연구](https://pmc.ncbi.nlm.nih.gov/articles/PMC4786863/)

같은 측정시각의 온도·습도와 여러 센서는 하나의 수평 이동 단계를 공유한다. 시계/부팅·입력 소스·원점·항법 연속 구간 또는 위치 출처가 바뀌거나, 위치/속도를 확인할 수 없거나, 관측 간격이 `spatial_gap_ms`를 넘으면 경로 연속성을 끊는다. 역순 표본은 경로에 반영하지 않고 late 진단을 남긴다. 미상 관측은 공간 구간에 억지로 배정하지 않고 기존 원본/시간 요약에 남긴다. ESTGPS의 500ms 속도·상태 유효 조건과 최대 30초 수신기 DR 조건은 별도이며 공간 설정으로 늘리지 않는다.

목표 폭을 `L0=spatial_step_m`, 관측된 표본 간 수평 이동거리를 `d`라고 하면 적응 모드의 새 구간 명목 폭은 `L=max(L0,d)`를 기준으로 한다. 10.01m 간격을 불필요하게20m로 올림하지 않으며 실제 간격25m라면 폭도25m를 기준으로 한다. 폭 변경은 새 구간 경계에서 적용하고 이미 열린 구간의 폭·범위를 도중에 바꾸지 않는다. 모듈은 실제 폭과 명목 구간 범위, 표본이 실제 차지한 경로 범위와 진입 간격을 함께 보존한다. 폭 감소에는 연속된 두 구간 진입의 더 작은 간격 확인을 사용하고, 완료 레코드 큐가 포화하면 적응 모드에서 단일 간격 기준과 기존 폭의 두 배 중 큰 값으로 다음 폭을 넓히는 보호 정책을 사용한다(모듈 폭 상한 이내). 따라서 실제 명목 폭이 단일 간격 공식보다 클 수 있다. `spatial_adaptive=0`은 목표 폭을 유지하되 채우지 못한 간격을 표시한다.

표본이 없는 여러 구간을 건너뛰어도 빈 구간이나 가상의 경계 측정값을 만들지 않는다. 명목상 구간 폭이 일정해도 실제 대표 표본의 위치 간격이 꼭 그 폭과 같지는 않다. 표본 간격 초과와 경로 불연속은 진단으로 남기며 복원된 관측 수로 세지 않는다.

### 통계와 누적 revision

출력은 `mavlink_weather_v1.records[].spatial` 내부의 `schema: "mavlink_spatial_v1"` 객체다. `spatial_enabled=0`이면 이 추가 객체를 보내지 않는다. 구간마다 metric·message_id·sensor_id·sensor_instance·위치 출처를 구분하고 실제 대표 표본 하나와 n·평균·M2·최솟값·최댓값·시각 범위를 유지한다. 대표 표본의 값·시각·위치·품질은 함께 선택하며 평균을 그 표본의 위치에서 잰 값으로 바꾸지 않는다. 표본분산은 n≥2일 때 `M2/(n−1)`, n<2일 때 null이다. 미상 측정·항법 오차는 null이다.

진행 중 구간은 1초 전송 때 닫지 않는다. 새 표본이 있으면 같은 구간의 누적 revision을 보내고, 구간 경계를 넘으면 완료, 연속성을 잃으면 중단 상태로 마감한다. 전송 직전에 새 표본이 없는 진행 구간은 반복 갱신하지 않는다. JSON의 `state`는 `partial`·`complete`·`interrupted`를 구별한다. 누적 경로·구간 식별자는 `src/spatial.h`, 표현은 `src/spatial.c`의 `sp_batch_json`을 따른다.

| 공간 JSON 필드 | 의미 |
|---|---|
| `target_step_m`, `adaptive`, `gap_us` | 목표 폭·적응 사용 여부·관측 간격 한도; `gap_us`는 µs 십진 문자열 |
| `path` | 최신 경로의 known·path_id·bin_index·누적 s_m·effective_width_m |
| `counters` | 수신/공간 반영/미상/거부/지연/경로/완료/손실/간격 초과/운동 불일치 진단과 flags |
| `bins[]` | 완료·중단 레코드와 변경된 진행 레코드의 누적 snapshot |
| `continuity_key`, `path_id`, `bin_index`, `revision` | 정밀도를 보존하는 64비트 십진 문자열 |
| `metric`, `message_id`, `sensor_id`, `sensor_instance`, `source_kind` | 센서/물리량/위치 출처 그룹; GPS와 ESTGPS를 구별 |
| `width_m`, `interval_start_m`, `interval_end_m` | 구간의 실제 명목 폭과 경로상 경계(m) |
| `observed_start_m`, `observed_end_m`, `entry_step_m` | 실제 표본이 차지한 경로 범위와 진입 표본 간 이동거리(m) |
| `t_start_us`, `t_end_us` | 실제 표본의 부팅 µs 시각 범위, 64비트 십진 문자열 |
| `statistics` | n·mean·m2·variance·min·max; variance만 n<2에서 null |
| `representative` | 실제 표본의 값·시각·위치·속도·품질·미상 오차, 기존 온습도 표본 구조 |
| `flags`, `quality` | 경로 진단과 원본 품질 OR; 센서 정확도의 숫자 순위가 아님 |

서버의 구간 키는 **송신자/세션 + continuity_key + path_id + bin_index + metric/message_id/sensor_id/sensor_instance/source_kind**다. 같은 키에서 가장 큰 revision만 유지하고 낮거나 같은 revision 재전송은 중복으로 처리한다. 누적 n·평균·M2는 새 revision으로 **교체**하며, partial 여러 개와 마지막 complete를 더하면 중복 집계다. 서로 다른 완료 구간을 합칠 때만 n·평균·M2의 결합식을 적용한다. 요청의 batch/seq 재시도 중복 제거와 구간 revision 교체는 각각 필요하다.

진행 구간의 누적 통계는 고정 상태로 유지하고 원본은 기존 배치 계약에 따라 ACK 후 해제한다. 송신 작업자는 snapshot의 불변 사본을 가져가 HTTP 204를 받을 때까지 재시도해야 한다. snapshot을 만들었다는 이유만으로 운송 계층의 확인되지 않은 사본을 버리지 않는다.

### 표본 밀도와 물리 응답의 한계

균일한 속도 `v`, 원본 표본률 `fs`에서 원본 간격은 `Δs≈v/fs`, 목표 폭 L의 기대 표본 수는 `n≈fs*L/v`다. 목표 간격 이하의 실제 표본을 얻으려면 대략 `fs≥v/L`이 필요하지만 이것만으로 공간 기상장을 복원할 수는 없다. 관측이 느린 센서를 수집기에서 복제·보간하여 표본률을 높이지 않는다. 센서 출력 설정 자체를 속도로 자동 변경하지 않으며 가용 원본을 모두 받아 공간 구간 선택/폭을 적응한다. 느리게 이동해 n이 증가해도 자기상관과 실제 시간/공간 변화가 섞이므로 정확도 향상이나 `표준오차=표준편차/√n`를 자동 주장하지 않는다. [NIST 자기상관 평균 불확도](https://www.nist.gov/publications/calculation-uncertainty-mean-autocorrelated-measurements?pub_id=151800), [NIST 독립 반복관측 조건](https://physics.nist.gov/cuu/Uncertainty/typea.html)

SHTC3 공개 데이터시트 v4(2022-12) 표5의 normal 변환은 typ10.8/max12.1ms, low-power 변환은 typ0.7/max0.8ms다. 이 값은 온습도 한 쌍의 변환 시간이며 정격 최대 반복주파수나 대기 응답속도가 아니다. 문서에는 보증 최대 반복 Hz가 별도로 없고 현재 PX4 드라이버의 10Hz는 이 프로젝트 설정이다. 표1의 RH 응답은 τ63%=8s(25°C·기류1m/s), 표2의 온도 응답은 τ63%=<5~30s이며 기판 열전도·실장에 따라 달라진다. [제조사 데이터시트](https://sensirion.com/resource/datasheet/shtc3)

1차 지연 모형 `τ·dy/dt+y=x`와 일정 속도를 가정하면 공간 기억길이는 대략 `vτ`다. 예를 들어 RH τ=8s·속도20m/s에서는 약160m이며 이는 모델에 따른 추정이다. **10m bin은 분류 폭이고, 10m의 독립적인 물리 관측 해상도나 위치 정확도 보증이 아니다.** 알려진 센서 시정수·외함·통풍·복사열 교정 없이 역필터/자동 위치 이동을 적용하지 않는다.

### 계산과 저장공간 계약

경로 한 단계는 O(1), 그룹 집계는 최대16개 고정 그룹 검색 O(16)이며 Welford 평균/M2 갱신은 그룹당 O(1)이다. 이동거리가 커져도 건너뛴 bin마다 반복하거나 표본을 생성하지 않는다. 추가 CPU/s 예산은 `f_time*C_path + f_weather*(C_group+C_statistics) + f_seal*(C_snapshot+C_JSON)`으로 구분한다. 여기서 `f_time`은 서로 다른 유효 온습도 측정시각 수, `f_weather`는 실제 온습도 관측 수, `f_seal`은 레코드 확정 빈도다. UDP를 켜면 표본마다 확정하므로 JSON·snapshot 생성 비용도 관측 빈도에 비례한다. 같은 시각의 온도·습도에 경로를 두 번 더하지 않는다. 확정 시 JSON을 만들고 UDP 비차단 송신을 시도하며 HTTP 응답은 별도 작업자가 기다린다. 이는 OS/I/O·mutex·소켓을 포함한 WCET 보장이 아니다.

현재 헤더의 공간 상태는 최대16개 진행 그룹·128개 완료 레코드와 최대144개 snapshot 레코드로 제한한다. 공간 원본을 별도로 무한 복제하지 않는다. 정지해 진행 구간이 오래 열려도 n·M2·대표 표본 등 고정 상태만 갱신한다. 공간 레코드 초과는 손실 계수로 남기며, raw/큐 용량과 전체 고정 메모리의 최종 수치는 `--sizes`와 해당 버전 검증 기록을 따른다.

## GPS/ESTGPS 위치 정책 — 0.9.0 이후

모니터·상태 보고의 마지막 위치 표시와 관측에 위치를 연결할 수 있는 조건은 분리한다. 마지막으로 수락한 실제 좌표와 당시 GPS/ESTGPS 출처, 자체 수신 시각을 보존하여 1Hz 항법에서도 표시가 UNKNOWN과 교대하지 않는다. 표시의 LIVE/STALE는 `link_timeout_ms` 기준이며 명시적인 추정기 무효 응답은 INVALID로 표시한다. 새 입력·부팅 세대에서는 이전 위치를 지운다. 아래500ms 규칙과 공간·시간 보간의 엄격한 제한은 측정 데이터에 그대로 적용한다.

`GLOBAL_POSITION_INT`는 FC가 융합한 전역 항법 결과이고 `GPS_RAW_INT`는 GPS 센서 상태다. 둘을 같은 GPS 원시 좌표로 취급하지 않는다. [공식 GLOBAL_POSITION_INT](https://mavlink.io/en/messages/common.html#GLOBAL_POSITION_INT), [GPS_RAW_INT](https://mavlink.io/en/messages/common.html#GPS_RAW_INT).

1. 수신기 host 시각과 원천 boot 시각 양쪽에서 최근 2초 안의 `GPS_RAW_INT` fix 3..8 및 검증 가능한 GPS source 시각을 요구한다. 여기에 최근 2초의 `ESTIMATOR_STATUS` 절대 수평/수직 위치 비트 `16|32`가 모두 유효하면 GPI를 `GPS`로 분류한다.
2. GPS 확인 없는 FC 전역 항법은 `ESTGPS`다. 최근 status가 절대 위치 비트 `16|32`를 명시적으로 부정하거나 glitch/accel 오류를 알리면 거부한다. status가 없거나 오래된 경우에도 기존 EKF 전역 입력은 ESTGPS로 보존하지만, 이는 위치 상태를 검증했다는 뜻이 아니며 오차는 미상이다. FC가 계속 수락 가능한 GPI를 주는 동안 수신기가 별도 속도 변위를 더하지 않는다. GPI 최근성은 500 ms로 검사하며, 유효한 새 GPI가 계속 들어오는 경로에 수신기 DR의 30초 만료를 적용하지 않는다.
3. GPI가 끊긴 경우에만 마지막 유효 GPS 기준점부터 `LOCAL_POSITION_NED` 지상 속도를 NED→ENU `(vy,vx,-vz)`로 바꾸어 적분한다. 기준점 없는 local x/y/z를 전역 좌표로 간주하지 않고, heading/yaw × airspeed를 사용하지 않는다.

수신기 DR의 한 단계는 `p[k+1] = p[k] + (v[k]+v[k+1])/2 * Δt`인 3축 사다리꼴 적분이다. 속도 표본의 source/host 간격이 500 ms를 넘거나 역전·중복·비유한 값·속력 500 m/s 초과면 그 구간을 적분하지 않는다. `ESTIMATOR_STATUS`는 host와 source 양쪽에서 2초 이내여야 하고 수평/수직 속도 비트 `2|4`가 모두 필요하다. 기준 GPS로부터 source/host 경과가 30초를 넘으면 중단한다. 늦거나 시계와 맞지 않는 GPS/status는 기준 boot 시계나 세션을 재설정하는 용도로 사용하지 않는다. ms 항법 시각과의 최대 1 ms 미래 양자화 허용은 실제 시각 정확도 보증이 아니다.

| ESTIMATOR_STATUS 비트 | 이 구현의 처리 |
|---|---|
| `2`, `4` 수평·수직 속도 | 수신기 속도 적분에 모두 필요 |
| `16`, `32` 절대 수평·수직 위치 | GPS 분류에 모두 필요; 최근 status가 이를 부정하면 FC 전역 위치도 거부 |
| `128` CONST_POS_MODE | 정상 정지 GPS/GPI는 허용하지만 수신기 속도 적분은 금지 |
| `1024` GPS_GLITCH, `2048` ACCEL_ERROR | 해당 상태의 위치/속도 추정을 거부 |

이는 수집기의 보수적인 수락 정책이다. 공식 비트 의미는 [ESTIMATOR_STATUS_FLAGS](https://mavlink.io/en/messages/common.html#ESTIMATOR_STATUS_FLAGS)를 따른다. 수신기 적분이 멈췄을 때 새 GPS 기준점·유효한 FC GPI와 연속 속도 조건 없이 누락 구간을 0 속도로 메우거나 이전 적분을 임의 재개하지 않는다.

항법 ring은 최대 32개이며 GPS/ESTGPS 출처와 앵커 시각을 병렬 고정 배열로 보관한다. 보간에 추정 항법이 하나라도 포함되면 결과도 ESTGPS다. 원본 온습도는 위치가 없어도 보존한다. 위치 출처 미상은 UNKNOWN, 위치/속도/오차/경과는 해당 필드의 null 규칙을 따른다. 수신기 적분 오차를 시간만으로 수치화하지 않으며 알려진 항법/시간 오차가 없으면 `navigation_uncertainty_m`·`position_uncertainty_m`은 null이다. 경과 30초 이내라는 조건은 정확도 상한이 아니다.

TUI의 `NO_ANCHOR`는 사용할 전역 항법과 이전 유효 GPS 기준점이 모두 없는 상태다. JSON `position_source`의 새 종류가 아니라 `UNKNOWN`·좌표 null에 대한 진단 표시다. `last_fix_age`는 마지막 유효 GPS의 source boot 경과이고, 알 수 없으면 unknown이다. 실내 fix 0 상태에 임의 원점/0 좌표를 만들지 않는다.

## 키–값, 위치와 시각

NAMED_VALUE_FLOAT/INT의 이름을 key로 사용하며 새 키는 자동 등록한다. 표준 wire value는 float32/int32이며 NAMED_VALUE_* 이름은 wire 규격상 최대10바이트다. 단위는 메시지에 없어 unit_map을 쓰며 미지정은 unknown이다. 키 최대31자 `[A-Za-z_][A-Za-z0-9_.-]*`, 단위 최대15자 ASCII 규약, 활성 소스당 기본 최대16키를 사용한다. 같은 키의 단위는 한 실행 설정에서 고정한다.

SCALED_PRESSURE/2/3 및 HIGHRES_IMU의 유효 압력·온도 필드도 처리한다. HIGHRES_IMU의 서로 다른 instance는 키를 구분한다. 알 수 없는 메시지·서명 검증 없는 signed 프레임은 수와 사유를 표시하고 넘어간다. 새로운 값 이름은 코드 변경이 필요 없지만, 정의가 없는 새 wire 메시지를 자동 해독하지는 않는다.

GLOBAL_POSITION_INT의 boot-ms와 위치/속도를 위 정책에 따라 GPS/ESTGPS로 분류해 사용한다. NED 속도는 ENU `(vy,vx,-vz)`로 바꾼다. 위치는 지정 또는 첫 유효 WGS84 위·경도 원점의 지역 반경 투영이고 고도는 MSL 차이다. 이는 지역 근사이며 geoid 변환이 아니다. 원점 불명인 LOCAL_POSITION_NED 좌표를 같은 좌표계라고 가정하지 않는다.

같은 세션의 검증·분류된 양쪽 항법이 있을 때만 선형 보간하고 최대200ms 간격을 허용한다. 추정 출처는 보간 후에도 유지한다. `A*Δt²/8` 항은 가속도 가정 A가 실제 상한일 때만 상한이다. 항법이 없어 연결할 수 없는 관측은 보류 후 no_navigation으로 집계한다. 고속 NAV 이력에서 밀려나도 이미 계산한 실제 보간 결과는 관측에 보존한다.

센서 boot-ms 양자화의 상대 오차는1000µs로 둔다. GPS_RAW_INT의 유효 h_acc/v_acc가 없으면 위치오차는 미상이다. HDOP를 미터 오차로 오인하지 않는다. 미상은 raw와 통계에 null이며 position_error_n이 오차를 알 수 있는 유효 값의 수다.

SYSTEM_TIME의 원시 boot/UTC 쌍은 raw_clock_anchor로 보존한다. 기본 오차 미상에서는 관측별 UTC를 만들지 않는다. 지정한 오차가 있으면 최근16anchor 중심화 OLS와 ±500ppm 필터, 마지막 anchor 뒤10초 외삽 한도로 UTC/오차를 추정한다. 추정오차는 입력 오차·RTT/2·잔차·드리프트 가정의 합이며 교정된 절대상한이나95%신뢰구간이 아니다.

MAVLink 입력은200ms 제한 재정렬 뒤 시각순으로 처리한다. 수신 후200ms 대기는 재정렬만 끝내며 측정시각·좌표·원천 boot watermark를 만들어 내지 않는다. 너무 늦은 입력, 중복, 다른 컴포넌트 시계는 별도 거부 수로 표시한다. 32bit-ms wrap은 확장하고 큰 boot 후퇴는 새 epoch로 분리한다. 재연결·부트 재설정 시 미확인 이전 묶음의 ID·원점·시각은 바꾸지 않는다.

## PX4 측 구현과 저장소 경계

`HyRUS_EnvData_Px4`는 I²C 센서 읽기·검증, sensor_hygrometer uORB 발행, 공식 SENS_ATMOS 송신, 보드별 등록 패치·펌웨어 빌드·검증을 관리한다. 기존 `src/px4/`와 `support/px4/` 소스는 그 별도 작업 폴더로 분리했다. Router 빌드·설치·배포에는 이 펌웨어 구현을 포함하지 않는다.

Router의 `mavlink_input.c`, `vendor/mavlink`, MAVLink 수신·온습도 회귀 검사는 호스트 프로토콜 처리에 필요하므로 이 저장소에 유지한다. 새로운 캐리어는 [입력 규약](PX4_MAVLINK_CONTRACT.md)의 메시지 ID·단위·시각·센서 식별 조건을 먼저 합의하고, PX4 송신 검사와 Router 수신 검사를 각각 추가한다. 펌웨어를 갱신했다는 이유로 Router가 알 수 없는 wire 구조를 추측하지 않는다.

## 혼합 관측의 전송 묶음

운용 모드에서 기존 key–value와 온습도 레코드가 함께 대기하면 기본1초 요청 주기를 유지하면서 `mavlink_router_bundle_v1`으로 묶는다. 공간 집계는 온습도 payload의 `records[].spatial`에 포함되며 별도의 bundle kind가 아니다. 아래 weather payload는 envelope 구조를 줄여 쓴 예이며 0.10.0 서버는 그 안의 추가 공간 객체도 처리해야 한다. 기본 최대4개이며, 혼합 시 아래의 최소2조각 규칙을 따른다. 각 payload는 원래의 완전한 envelope다. 한 종류만 대기하면 기존 schema3 또는 mavlink_weather_v1을 직접 보낸다.

```json
{"schema":"mavlink_router_bundle_v1","records":[
  {"kind":"telemetry","payload":{"schema":3,"stream":"...","batches":[]}},
  {"kind":"weather","payload":{"schema":"mavlink_weather_v1","stream":"...","records":[]}}
]}
```

위 예시는 구조만 나타내며 생략된 식별자·원점·원본·통계는 실제 payload에 들어간다. 서로 다른 소스 세션을 한 요청으로 합치지 않는다. 각 내부 자료의 `(stream,session,batch_id)` 또는 `(stream,session,seq)`를 중복 키로 유지한다. 서버는 내부 레코드까지 모두 수락한 경우에만204로 답해야 한다. TCP21000 선택 시험 서버는 heartbeat/weather 시험 envelope를 검사하는 진단 도구이며 일반 schema3·혼합 bundle 수집 서버를 대신하지 않는다.

두 종류가 함께 대기할 때는 `max_windows=1`이어도 최소2개의 종류별 조각을 한 요청으로 묶는다. 한 종류만 있으면 기존 한도를 유지하고 기본값은4다. HTTP는 전송 주기가 되면 그 시점의 대기 항목을 고정하되 각 요청 완료 후 `http_min_interval_ms`만큼 기다려 요청을 분산한다. 기본500ms는 측정·상태·재시도·종료와 설정 재시작의 대기열 전송 전체에 공유하며, 설정 재시작도 직전 요청의 대기를 초기화하지 않는다. 두 종류의 전송이 모두 준비되면 측정과 상태에 차례로 기회를 준다. 새 관측은 다음 라운드로 넘기고 미확인 항목은 보존한다.

401·403·429에는 공통 대기 시간을1,2,4,8,16,30초로 늘린다. 204 성공만 이 추가 대기를 초기화한다. 503·연결 오류 등 다른 실패는 대기를 늘리지는 않지만 기존 대기를 유지하고, 측정 실패에는 `send_interval_ms`도 적용한다. 실제 대기는 이 추가 대기와 `http_min_interval_ms` 중 큰 값 이상이다. 이 대기는 HTTP 작업자의 다음 요청을 미루며 MAVLink 수신이나 UDP 백업을 멈추지 않는다. `Retry-After`는 해석하지 않는다. 403은 서버 정책·권한에 따른 거부일 수 있으므로 서버 로그 없이 인증 문제나 요청률 문제로 확정하지 않는다. 실제 입력량이 서버 처리량보다 크면 RAM FIFO가 상한에 도달할 수 있으며, 이때 새 TCP 항목의 손실을 표시한다. 원본은 디스크로 흘려 쓰지 않는다.

## UDP 즉시 송신과 TCP 메모리 보존

`udp_backup_enabled=1`이면 해독·검증을 거쳐 수락한 일반 관측·온습도 표본마다 레코드를 확정한다. ID·세션·원점·시계·표본을 한 번 정하고 JSON을 인코딩한 뒤, 비차단 UDP 소켓으로 바로 송신을 시도하고 같은 JSON을 TCP FIFO에 넣는다. 별도의 UDP 송신 대기열을 쌓거나 HTTP 응답·`send_interval_ms`를 기다리지 않는다. `udp_backup_host=auto`는 HTTP URL 호스트, 포트0은 TCP 포트+1이며 각각 명시해 바꿀 수 있다. HTTP 경로·query·Bearer 토큰은 UDP에 포함하지 않는다. HTTPS도 UDP를 암호화하지 않는다. UDP 비활성화 시에는 기존 측정 시간 구간·전송 주기의 배치 수집을 유지한다.

표본별 확정은 실시간 `schema:3`과 온습도에 적용한다. CSV `schema:2`는 `window_us`로 중복 제거하므로 기존 측정 시간 구간의 배치를 유지하고, 확정된 배치만 UDP로 보낸다. 하트비트 시험·주기 상태는 자체 생성 주기를 따른다.

TCP FIFO는 필요할 때 가변 크기 노드를 할당하며 인코딩 JSON과 노드 관리 정보의 합계를 `tcp_buffer_max_mb`로 제한한다. 기본1024MiB는1GiB(1,073,741,824B)이며 처음부터 그 크기를 예약하지 않는다. 이 한도는 현재 수집 배치·UDP/HTTP 작업 버퍼·스택·OS/TLS 메모리를 포함하지 않는다. HTTP204로 확인된 FIFO 앞부분만 해제한다. 한도 도달이나 할당 실패에는 기존 미확인 자료를 보존하고 새 TCP 항목의 거부 수를 올리며 UDP 송신 시도는 유지한다. 종료·전원 차단 뒤 복구할 영속 큐는 없다.

UDP 송신은40B `MTRU` v1 헤더와 최대1160B JSON 조각을 사용한다. ACK·재시도는 없고, 소켓 미준비·커널 버퍼 포화·인코딩/송신 실패는 누락·오류 계수에 반영한다. `sent`는 로컬 소켓에 모든 조각을 넘긴 결과다. 서버는 양쪽 경로의 같은 개별 ID를 중복 제거하고 공간 구간의 큰 revision만 유지한다. UDP 헤더의 nonce/전송 순번은 관측 ID가 아니다. 상세 헤더·재조립·만료는 [UDP 백업 규약](UDP_BACKUP.md), 저장·응답은 [서버 API](SERVER_API.md#2-통신-방식)를 따른다.

## 출력과 서버 확인 — schema3

기존 key–value 관측만 담은 실제 MAVLink의 최상위 JSON은 schema3다. 온습도 및 혼합 관측 envelope는 위 절을 따른다. stream/session, ENU origin_wgs84, source{sysid,compid,epoch,altitude_datum:MSL}, field_order, 누적 loss, clock_model, 선택 raw_clock_anchor, batches를 포함한다. 세션은 설정 session에 OS 난수 기반 실행 ID와 소스 세대를 붙여 부팅/연결 시계를 분리한다.

각 batch는 **불변 batch_id**와 측정시각 window_us, n/raw_n/raw_dropped/key_n/raw_included, 키별 stats·원본·해당 묶음의 clock_model/선택 raw_clock_anchor를 갖는다. 같은 측정시각1초 구간이 여러 batch로 나뉠 수 있다. UDP 활성화 시에는 표본마다 나뉘므로 서버는 `(stream,session,batch_id)`로 중복 제거하고 window_us로 재집계한다. schema2 CSV의 `(stream,session,window_us)` 중복 키를 schema3에 적용하면 정상 후속 묶음을 잃는다. 재시도 중 개별 봉인 JSON은 유지되지만 전체 요청의 선택 항목·bundle 포장은 달라질 수 있으므로 요청 전체 해시로만 중복을 판단하지 않는다. 전체 수신 계약은 [서버 API](SERVER_API.md)를 따른다.

서버는 요청의 모든 묶음을 영속 저장/영속 큐 수락한 뒤 **204**를 반환한다. 이미 수락한 중복에도204를 반환한다. 200/202/오류/응답 단절은 자료와 ID를 유지해 다시 보낸다. TCP ACK는 이 애플리케이션 확인을 대체하지 않는다.

raw는 다음17필드다. t_us/window_us/utc_us와64bit 누적 계수는 정밀도를 위해 십진 문자열이다. 미상 값·오차·UTC는 null이다.

```text
t_us,seq,quality,key,unit,east_m,north_m,up_m,ve_mps,vn_mps,vu_mps,value,time_uncert_us,nav_age_us,pos_error_m,utc_us,utc_uncert_us
```

seq는 키별 중계 관측 순번이며 MAVLink 공용8bit헤더 순번이 아니다. quality는 원본 품질 비트와 위치오차 미상 등의 플래그다. 값·좌표는 재시도 중 재계산하지 않는다. 최상위 clock_model/raw_clock_anchor는 첫 묶음 기준의 진단이다. 각 batch에도 마감 당시 모델·원시 anchor를 따로 보존한다. 이 진단으로 raw를 재계산하지 않으며, 각 raw의 UTC/오차는 해당 관측을 처리한 당시의 추정이다.

통계는 키별 Welford 평균·표본분산 M2/(n−1)·min/max, 상대시각 오차와 추정위치 오차 요약이다. n<2 분산은null, position_error_n0이면 위치오차 요약도null이다. 평균은 표본 가중이며 거리/시간 가중이나 센서 정확도 신뢰구간이 아니다. 여러 batch의 평균을 단순 평균하지 말고 n·평균·M2로 결합해야 한다.

TCP 큐는 기본1GiB의 미확인 JSON·노드 관리 정보, 요청은 기본최대4묶음, 일반 관측 원본은 최대512행/묶음이다. 원본초과분도 통계에는 포함하고 누락 수를 기록한다. 큐가 가득 차면 새 묶음을 버리고 전송 중 자료를 덮어쓰지 않는다. --summary는 원본을 생략해 위치별 원본 복원에 불충분하다.

수신·관측처리, HTTP 송신, TUI는 분리되어 있다. Windows WebSocket은 고정 수신 버퍼 작업자를 추가로 쓴다. 포트 검색은 프레임마다 수행하지 않는다. 탐색 경로는2개로 고정하며 각 파서와 transport 저장공간을 시작 전에 확보한다. TCP 보존 노드는 확정 레코드마다 동적으로 할당하지만 새 스레드나 작업 풀을 만들지는 않는다. 탐색 중 입력 확인 비용은 경로 수2에 비례하고 선택 뒤에는 한 경로만 읽는다. 파서 O(bytes×log(known messages)), device최대32·pending512·nav32이며 TCP 보존량은 설정한 바이트 상한을 따른다. pending 정렬·보간은 고정 상한이 있으나 하드 WCET를 주장하지 않는다. `--sizes`의 고정 수집/송신/입력/경로 저장공간 수치에는 동적 TCP FIFO와 스택·OS·TLS·libcurl 내부 메모리가 포함되지 않는다.

## 하트비트·좌표 시험 모드와 TCP 서버

운용 실행 파일에 `--test-heartbeat`를 지정하면 `schema:3` 관측 전송 대신 진단용 `mavlink_router_test_v1` JSON을 HTTP POST로 보낸다. 기본 주소는 `http://127.0.0.1:21000/ingest`이며 `--server-url URL`이 있으면 그 주소를 사용한다. 기존 설정의 입력 포트·시간 제한·전송 주기는 읽되, 시험용 CLI 주소를 설정 파일에 자동 저장하지 않는다. 정상 수집으로 돌아가려면 `--test-heartbeat` 없이 실행한다.

```json
{
  "schema": "mavlink_router_test_v1",
  "stream": "env1",
  "records": [{
    "seq": 1,
    "source_generation": 1,
    "session": "auto.run_id.1",
    "source": "udp://127.0.0.1:14540",
    "heartbeat": {
      "sysid": 1, "compid": 1, "type": 2, "autopilot": 12,
      "base_mode": 128, "custom_mode": 0, "system_status": 4,
      "received_monotonic_ms": 12345
    },
    "position": {
      "lat_deg": 37, "lon_deg": 127, "alt_msl_m": 100,
      "boot_ms": 5000, "received_monotonic_ms": 12300
    },
    "coordinate_age_ms": 45
  }]
}
```

`seq`는 실행 내 전역 증가 정수다. 재연결·기체 재부팅은 `source_generation`과 `session`으로 구분한다. `heartbeat.received_monotonic_ms`와 위치의 같은 이름 필드는 **수집기 호스트의 단조 시계**이며 GPS UTC가 아니다. `position.boot_ms`는 해당 GLOBAL_POSITION_INT의 원천 부팅 경과시각이다. 위·경도는 deg, 고도는 MSL m다. GPS_RAW_INT의 다른 좌표로 GLOBAL_POSITION_INT 위치를 바꾸면서 이전 boot/수신시각을 붙이지 않는다.

좌표는 해당 입력·부팅 구간의 최신 수락 항법이며 위 GPS/ESTGPS 정책을 따른다. `coordinate_age_ms`는 이 레코드의 수집 시각과 항법 수신시각의 차이다. 하트비트의 측정 위치라고 보간하거나 센서 동시 측정이라고 주장하지 않는다. 항법이 없거나 위 항법 유효성/500ms 최근성 기준 또는 `link_timeout_ms`를 벗어나면 `position`과 `coordinate_age_ms`는 모두 null이다. 소스 시계가 초기화된 뒤에는 새 하트비트를 받아야 새 구간의 시험 레코드를 만든다.

선택한 기기가 식별되고 새 하트비트가 있을 때만, `send_interval_ms`당 최대 한 레코드를 넣는다. 이미 보낸 하트비트를 새 레코드로 반복하지 않는다. 원본 관측·커스텀 키·통계는 시험 JSON에 넣지 않는다. 동일한 메모리 한도와 요청당 기본 최대4개 규칙을 사용하고 HTTP204 전에는 레코드와 순번·좌표 나이를 보존해 재시도한다. 서버는 `(session,seq)`로 재전송을 구분할 수 있다.

선택 빌드 `BUILD_TEST_SERVER=ON`은 `tools/mavlink_telemetry_data_router_test_server[.exe]`를 만든다. 배포물에서도 루트 `tools/`에 두며 운용 두 프로그램의 `bin/`과 구분한다. 기본 bind는 `127.0.0.1:21000`이다.

시험 서버 TUI는 라우터와 동일한 `monitor_frame` 행 비교·출력 구현을 사용한다. 전체 지우기 없이 바뀐 행을 한 번에 출력하고, 줄 끝의 남은 문자만 지운다. 제목·카운터·로그를 모두 현재 창 너비 안에 자르며, 창 크기 변경에 맞춰 표시할 로그 수를 조절한다. 요청이 없거나 수신 대기 중에도 주기적으로 화면을 확인하여 `run_elapsed`와 창 크기를 갱신한다. 정상 레코드의 화면 반영은 HTTP204 전에 수행하며, `--plain`의 레코드 JSON 출력은 기존과 같다.

| 서버 옵션 | 의미 |
|---|---|
| `--bind IPv4` | 기본 127.0.0.1. 원격 수신에는 해당 인터페이스 또는 0.0.0.0 |
| `--port N` | 기본 TCP 21000, 1–65535 |
| `--plain` | TUI 대신 완전한 레코드 JSON 한 줄씩 stdout에 표시 |
| `--duration-ms N` | 기본 0 무기한, 양수는 실행 제한 |
| `--max-requests N` | 기본 0 무제한, 양수는 TCP 연결 처리 횟수 제한 |

서버는 POST `/ingest`와 Content-Length를 처리한다. 헤더 최대8KiB·본문 최대512KiB·JSON32,768토큰/깊이32·레코드 최대64개·연결별 최대5초로 제한하며 온습도/공간 및 하트비트 시험 필드를 검사한다. 일반 schema3·혼합bundle·CSV2는 이 선택 시험 서버의 수락 범위에 포함하지 않는다. 잘못된 JSON·중복 Content-Length·Transfer-Encoding·범위 밖 크기는204로 수락하지 않는다. 정상 레코드가 화면 또는 stdout에 출력된 뒤204를 보낸다. 메모리 링 로그만 유지하고 관측 파일이나 영속 큐는 만들지 않으므로 **이 시험 서버의204는 영속 저장 확인이 아니다.** 운영 서버는 [서버 API](SERVER_API.md)의 전체 schema·중복 제거·커밋 규칙을 구현한다.

## 빌드와 개발 검사

CMake3.21+, Ninja, C99 x64 컴파일러가 필요하다. Windows WinHTTP/bcrypt/ws2_32, Linux libcurl/pthread를 사용한다. 기본 MinSizeRel은 검사 OFF이며 Windows/Linux 각각 두 운용 파일을 만든다.

| 작업 | Windows | Linux |
|---|---|---|
| 구성/빌드 | windows-x64 | linux-x64 |
| 선택 검사 구성/빌드/CTest | windows-x64-tests | linux-x64-tests |
| Debug 구성/빌드 | windows-x64-debug | linux-x64-debug |
| 패키지 빌드 | package-windows-x64 | package-linux-x64 |

각 이름에 `cmake --preset`, `cmake --build --preset`, `ctest --preset`을 맞춰 사용한다. package는 build 명령만 쓴다. 검사 프리셋은 같은 build/<OS>를 재사용하고 tests/에 결과를 둔다. 기본 프리셋 재구성은 검사를 OFF로 돌리지만 이전 검사 파일은 삭제하지 않는다.

컴파일러·Ninja 경로는 -DCMAKE_C_COMPILER, -DCMAKE_MAKE_PROGRAM 또는 Git 제외 CMakeUserPresets.json에서 지정한다. 서로 다른 컴파일러의 캐시를 섞지 않는다. 컴파일러의 x86-64 매크로와 포인터8바이트를 확인하며 Intel/AMD에 같은 바이너리를 쓴다.

Windows에서 Linux를 교차 빌드할 때는 LLVM-MinGW Clang22/LLD, Ninja, Python3.9+를 PATH에 두고 다음을 수행한다. 실제 Linux에서 native 빌드할 때는 이 절차가 필요 없다.

```powershell
python -m pip install --target build/toolchains/python-deps zstandard
$env:PYTHONPATH = "$PWD/build/toolchains/python-deps"
python cmake/toolchains/prepare-linux-x64.py
$python = (Get-Command python).Source
cmake -S . -B build/linux-x64 -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-x64-cross.cmake `
  -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=OFF `
  "-DTB_LINUX_PYTHON=$python" `
  "-DCMAKE_INSTALL_PREFIX=$PWD/build/stage/linux-x64"
cmake --build build/linux-x64 --parallel 2
cmake --build build/linux-x64 --target package
```

공식 Ubuntu22.04 패키지를 HTTPS로 받고SHA256를 확인해 build/toolchains에 추출한다. OS 패키지 설치 스크립트를 실행하지 않고 sysroot를 배포물에 넣지 않는다. 생성 ELF는 glibc2.34+와 OpenSSL 계열 libcurl.so.4를 사용한다. Windows에서 만든 Linux TGZ의 실행 권한도0755로 보정한다. 교차 CMake 캐시를 Linux native 캐시로 재사용하지 않는다.

선택 CTest는 코어·시계·설정·MAVLink 검사를 실행한다. 구성 시 Python3가 있으면 WebSocket/UDP transport·실제 router 통합 검사도 등록하며, 교차 빌드에서는 타깃을 실행하지 않는다. 추가 회귀는 다음과 같다(Linux는 실행 파일 .exe 없이 경로를 바꾼다).

```powershell
python src/tests/test_core.py --skip-build
python src/tests/test_sender.py
python src/tests/test_confirmation.py
python src/tests/test_monitor.py
python src/tests/test_sender_limits.py
python src/tests/test_mavlink_router.py
python src/tests/test_udp_router.py
python src/tests/test_auto_input.py
python src/tests/test_test_stream.py build/windows-x64/mavlink_telemetry_data_router_headless.exe build/windows-x64/tools/mavlink_telemetry_data_router_test_server.exe
```

`test_auto_input.py`는 UDP와 다른 입력의 우선 선택·끊김 후 전환·소스 분리를 실제 실행 파일로 확인한다. POSIX에서는 가상 tty도 사용한다. `python3 src/tests/test_monitor_connection.py build/linux-x64/mavlink_telemetry_data_router_interactive`는 Linux 가상 터미널에서 노란색 탐색→초록색 식별 연결→시간 초과 후 노란색 전환과 종료 복원을 검사한다. 이 두 검사는 독립된 운용 프로그램을 추가하지 않는다. `test_test_stream.py`는 시험 모드의 하트비트/좌표/재부팅 경계·204 재시도와 선택 TCP 시험 서버의 실제 요청 파서를 검사한다. 두 번째 서버 실행 파일 인자는 선택 사항이며, 생략하면 수집기 시험 모드만 검사한다.

GitHub/GitLab CI도 native검사 프리셋과 같은 경로를 사용한다. CI파일 작성과 원격 실행은 다르며 원격 CI는 여기서 실행하지 않았다. Git배포는 실제 저장소 화면에서 복사한 clone URL로 origin을 설정하고 push한다. 호스트 이름만으로 URL을 추측하지 않는다.

## 개발용 CSV 재생 — schema2

CSV는 실제 운용의 필수 입력이 아니다. 저장된 가상 관측으로 회귀 검사할 때 `input_mode=csv`를 명시하거나 다음 위치 인자를 사용한다.

```powershell
Get-Content .\examples\example.csv | .\build\windows-x64\mavlink_telemetry_data_router_headless.exe - env1 replay-unique 37 127 100
```

이 모드만 stdin EOF로 종료한다. 위치 인자의 원점은 기존 CSV 계약에 따라 타원체고이며 실제 MAVLink MSL 고도와 혼합하지 않는다. 관측 한 줄은15필드다.

```text
t_us,seq,quality,key,unit,value,east_m,north_m,up_m,ve_mps,vn_mps,vu_mps,time_uncert_us,nav_age_us,pos_error_m
```

전체t는비감소, 키별t와seq는증가해야 한다. 제어 행 `@clock,boot_us,utc_unix_us,gnss_uncert_us,rtt_us`와 `@watermark,W`를 사용한다. watermark는 앞으로t<W가없다는선언이며t==W는허용한다. CSV는 호스트의 조용한 입력만으로 구간을 마감하지 않는다. 다음구간/명시watermark/EOF가마감한다. schema2 중복키는(stream,session,window_us)다.

## QGC와 병행하는 UDP 경로

PX4 SITL 기본값은 GCS용 local18570→remote14550, API/Offboard용 local14580→remote14540이다(단일 기체 instance0). 수집기는14540, QGC는14550을 받으면 된다. SITL에 별도14551 출력이나 수집기→QGC 재전송이 필요하지 않다. 두 스트림은 모드별 메시지 구성이 다르며 커스텀 센서 메시지가 양쪽에 자동 복제된다고 가정하지 않는다. 다중 기체의 API/Offboard 목적지는14540부터14549까지 달라지므로 해당 입력 포트를 지정한다. [PX4 기본 포트](https://docs.px4.io/main/en/simulation/#default-px4-mavlink-udp-ports), [PX4 시작 설정](https://github.com/PX4/PX4-Autopilot/blob/main/ROMFS/px4fmu_common/init.d-posix/px4-rc.mavlink)

현재 확인한 WSL PX4의 API 경로는 WSL127.0.0.1:14580→WSL127.0.0.1:14540이다. Linux 수집기를 같은 WSL에서 실행하면 직접 받는다. Windows loopback의14540을 여는 것만으로 WSL loopback을 받을 수 없으며, 자동 WSL 어댑터는 구현하지 않았다. 기존 설정 파일에14551이 있으면 계속 그 값을 사용하므로 TUI에서 수정·저장 후 재시작해야 한다.

실제 USB 텔레메트리에서는 수집기가 시리얼을 소유하고 원시 MAVLink를 기본 `127.0.0.1:14550`의 QGC에 전달한다. QGC의 응답은 같은 시리얼 링크로 돌아간다. 라우터가 먼저 COM 포트를 선택해 `LOCKED`가 된 뒤 UDP 자동 연결이 켜진 QGC를 실행하면 시리얼 직접 연결과의 점유 경합을 피할 수 있다. 관측의 HTTP/HTTPS 전송은 별도 경로다.

다른 환경에서 QGC와 수집기가 같은 PC의 같은 유니캐스트 주소/포트를 공유하면 양쪽 전체 복사 수신을 보장할 수 없다. 수집기 포트만 바꾸면 송신 목적지가 자동 변경되지 않는다. 필요한 경우 QGC forwarding→별도 포트 또는 송신원/분배기의 별도 출력을 사용한다. QGC forwarding은 단방향이고 QGC가 처리한 메시지만 전달하므로 임의 dialect의 원시 byte mirror로 가정하지 않는다.

0.6.0 검증의 QGC 경로는 WSL PX4172.30.183.133:18570→Windows172.30.176.1:14550의 유니캐스트였다. 이 IP는 해당 실행 환경의 일시 값이며 프로그램에 고정하지 않는다. 당시 검증 시에만 AF_PACKET로 원본에 손대지 않고 메모리 복사해 별도14551로 넣었다. 이 임시 경로가 실행 파일의 자동 캡처 기능을 뜻하지 않는다. 원래 링크/포트/QGC 설정은 변경하지 않았고 관측 파일도 만들지 않았다.

[QGC Telemetry/forwarding](https://docs.qgroundcontrol.com/master/en/qgc-user-guide/settings_view/telemetry.html), [MAVLink Router](https://github.com/mavlink-router/mavlink-router), [Windows 포트 공유](https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse), [Linux SO_REUSEPORT](https://man7.org/linux/man-pages/man7/socket.7.html)를 참고한다.

## 근거와 적용 한계

[MAVLink HEARTBEAT](https://mavlink.io/en/services/heartbeat.html), [패킷 직렬화/CRC](https://mavlink.io/en/guide/serialization.html), [common 메시지](https://mavlink.io/en/messages/common.html), [TIMESYNC](https://mavlink.io/en/services/timesync.html)를 따른다. TIMESYNC만으로UTC가생기지는않는다. CRC표는고정된공식 c_library_v2출처이며 생성물라이선스는 src/vendor/MAVLINK_COPYING.txt에보존했다.

[OGC SensorThings](https://docs.ogc.org/is/18-088/18-088.html)의 관측시각/결과시각 구분, [SensorML](https://docs.ogc.org/is/23-000/23-000.html)의 시공간 기준틀 구분을 설계에 참고했다. 이 출력이 해당 API와 호환된다는 뜻은 아니다. 통계나 보간으로 미관측 물리장이 유일하게 복원되지는 않는다.

실제 센서 지연·GPS정확도·지오이드·서명키 검증과 타컴포넌트 시계 정렬은 장치 통합에서 확인해야 한다. 원본을 디스크에 자동 저장하지 않아 전원 차단/장기장애에는 손실이 발생한다. [검토 기록](REVIEW.md)에 실제 검증 범위를 구분했다.
