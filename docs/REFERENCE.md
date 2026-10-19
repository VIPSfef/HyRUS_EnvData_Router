# 기술 참조 — mavlink_telemetry_data_router 0.5.0

처음 실행할 때는 [README](../README.md)를 읽는다. 이 문서의 명령은 저장소 루트 기준이다. 기본 입력은 바이너리 MAVLink이고, CSV는 명시적인 개발용 재생 모드다.

## 자동 입력과 채널 선택

기본 `input_mode=mavlink`에서는 stdin을 읽지 않는다. 우선 포트가 있으면 그 포트, 지정 WebSocket, 자동 검색한 다른 직렬 포트를 순서대로 확인한다. `serial_port=auto`이면 WebSocket이 있으면 먼저 확인한 뒤 검색 포트를 확인한다. 한 번에 한 입력 채널을 유지한다.

Windows는 QueryDosDevice로 COM 후보를 찾고, Linux는 `/dev/serial/by-id/*`, `/dev/ttyACM*`, `/dev/ttyUSB*`를 탐색해 장치 중복을 제거한다. 검색 시 포트를 열지 않으며 실제 확인 순서에 도달한 포트만 연다. 통신 속도는 지정한 `serial_baud`를 사용한다.

WebSocket은 클라이언트로 접속하며 바이너리 데이터만 MAVLink 바이트 스트림으로 처리한다. ws/wss, 분할 프레임, ping/pong, 부분 TCP 읽기와 TLS 인증서 검증을 지원한다. 텍스트/JSON WebSocket은 별도 변환 없이는 입력하지 않는다.

MAVLink CRC가 맞는 프레임을 받으면 해당 채널을 유지한다. `probe_ms` 안에 없으면 다음 후보로, 연결 후 `link_timeout_ms` 동안 유효 프레임이 없으면 다시 탐색한다. 등록한 채널 밖의 임의 네트워크 주소를 검색하지 않는다.

기기는 HEARTBEAT의 system/component ID, MAV_TYPE, MAV_AUTOPILOT로 식별한다. component ID 숫자만으로 종류를 추측하지 않는다. 종류를 모르는 기기는 미상으로 표시한다. `target_system=0`, `target_component=0`은 첫 비행제어기를 선택한다. 다른 기기들은 목록에 남지만 시계가 다른 관측을 자동 합치지는 않는다.

설정한 포트와 WebSocket에는 연결 후, 자동 검색 포트에는 유효 MAVLink 확인 후 1Hz로 중계기 HEARTBEAT를 알린다. 비행 명령·arming·임무 명령은 보내지 않는다. source system255/component191, type ONBOARD_CONTROLLER/autopilot INVALID를 사용한다.

## 설정 전체

설정은 UTF-8 `key=value`이며 알 수 없는 키·중복·잘린 행·범위 밖 값은 시작 전에 거부한다. Windows 한글 경로를 지원한다. 변경은 원자적으로 저장하고 재시작 후 적용한다.

| 항목 | 기본 / 범위 |
|---|---|
| url | HTTP/HTTPS 주소 또는 JSON 미리보기 `-` |
| stream, session | ASCII 영숫자·`_-.`, 1–64자 |
| token_env | `TB_TOKEN`, 인증 환경변수 이름만 저장 |
| origin_lat/lon/alt | 고정 원점. 실제 MAVLink에서는 위·경도 deg, MSL m |
| origin_auto | 1: 첫 유효 GLOBAL_POSITION_INT를 원점으로 고정, 0: 지정값 |
| input_mode | mavlink 또는 csv |
| serial_port | auto, none, COM 이름 또는 tty 경로 |
| serial_baud | 115200. 지원: 9600/19200/38400/57600/115200/230400/460800/921600 |
| websocket_url | 빈 값 또는 ws/wss 주소, 최대2047자 |
| auto_scan | 1, 자동 직렬 포트 검색 |
| probe_ms | 2000, 100–60000 |
| link_timeout_ms | 5000, 100–3600000 |
| reconnect_ms | 1000, 100–60000 |
| target_system/component | 0 자동, 1–255 지정 |
| system_time_uncert_us | 4294967295 미상. 검증한 source SYSTEM_TIME 오차를 µs로 지정 |
| accel_bound_mps2 | 10, 0–1e6. 위치 보간용 가속도 가정 |
| duration_ms | 0 무기한, 양수는 입력 수집 제한 시간 |
| unit_map | 빈 값 또는 `temp:C;humidity:%;pm25:ug/m3` |
| send_interval_ms | 1000, 1000–60000. 재시도 포함 요청 시작 간격 |
| connect_timeout_ms/io_timeout_ms | 각각1000, 100–60000 |
| max_windows | 4, 요청당 최대4묶음 |
| refresh_ms | 1000, 250–10000 |
| idle_warn_ms/ack_warn_ms | 2000/5000, 250–3600000. 화면 경고 기준 |
| summary | 0 원본 포함, 1 원본 생략 |

토큰은 환경변수에 둔다. URL query는 화면에서 숨기지만 설정 파일에는 원문으로 저장된다. URL userinfo와 fragment는 거부한다. Windows timeout은 WinHTTP 단계별이며 Linux 총 HTTP timeout은 connect+2×io로 적용한다. DNS/TLS/OS까지 포함한 하드 실시간 상한을 뜻하지 않는다.

## 키–값, 위치와 시각

NAMED_VALUE_FLOAT/INT의 이름을 key로 사용하며 새 키는 자동 등록한다. 표준 wire value는 float32/int32이며 NAMED_VALUE_* 이름은 wire 규격상 최대10바이트다. 단위는 메시지에 없어 unit_map을 쓰며 미지정은 unknown이다. 키 최대31자 `[A-Za-z_][A-Za-z0-9_.-]*`, 단위 최대15자 ASCII 규약, 활성 소스당 기본 최대16키를 사용한다. 같은 키의 단위는 한 실행 설정에서 고정한다.

SCALED_PRESSURE/2/3 및 HIGHRES_IMU의 유효 압력·온도 필드도 처리한다. HIGHRES_IMU의 서로 다른 instance는 키를 구분한다. 알 수 없는 메시지·서명 검증 없는 signed 프레임은 수와 사유를 표시하고 넘어간다. 새로운 값 이름은 코드 변경이 필요 없지만, 정의가 없는 새 wire 메시지를 자동 해독하지는 않는다.

GLOBAL_POSITION_INT의 boot-ms와 위치/속도를 사용한다. NED 속도는 ENU `(vy,vx,-vz)`로 바꾼다. 위치는 지정 또는 첫 유효 WGS84 위·경도 원점의 지역 반경 투영이고 고도는 MSL 차이다. 이는 지역 근사이며 geoid 변환이 아니다. 원점 불명인 LOCAL_POSITION_NED 좌표를 같은 좌표계라고 가정하지 않는다.

실제 양쪽 항법이 있을 때만 선형 보간하고 최대200ms 간격을 허용한다. `A*Δt²/8` 항은 가속도 가정 A가 실제 상한일 때만 상한이다. 항법이 없어 연결할 수 없는 관측은 보류 후 no_navigation으로 집계한다. 고속 NAV 이력에서 밀려나도 이미 계산한 실제 보간 결과는 관측에 보존한다.

센서 boot-ms 양자화의 상대 오차는1000µs로 둔다. GPS_RAW_INT의 유효 h_acc/v_acc가 없으면 위치오차는 미상이다. HDOP를 미터 오차로 오인하지 않는다. 미상은 raw와 통계에 null이며 position_error_n이 오차를 알 수 있는 유효 값의 수다.

SYSTEM_TIME의 원시 boot/UTC 쌍은 raw_clock_anchor로 보존한다. 기본 오차 미상에서는 관측별 UTC를 만들지 않는다. 지정한 오차가 있으면 최근16anchor 중심화 OLS와 ±500ppm 필터, 마지막 anchor 뒤10초 외삽 한도로 UTC/오차를 추정한다. 추정오차는 입력 오차·RTT/2·잔차·드리프트 가정의 합이며 교정된 절대상한이나95%신뢰구간이 아니다.

MAVLink 입력은200ms 제한 재정렬 뒤 시각순으로 처리한다. 수신 후200ms 대기는 재정렬만 끝내며 측정시각·좌표·원천 boot watermark를 만들어 내지 않는다. 너무 늦은 입력, 중복, 다른 컴포넌트 시계는 별도 거부 수로 표시한다. 32bit-ms wrap은 확장하고 큰 boot 후퇴는 새 epoch로 분리한다. 재연결·부트 재설정 시 미확인 이전 묶음의 ID·원점·시각은 바꾸지 않는다.

## 출력과 서버 확인 — schema3

실제 MAVLink의 최상위 JSON은 schema3다. stream/session, ENU origin_wgs84, source{sysid,compid,epoch,altitude_datum:MSL}, field_order, 누적 loss, clock_model, 선택 raw_clock_anchor, batches를 포함한다. 세션은 설정 session에 OS 난수 기반 실행 ID와 소스 세대를 붙여 부팅/연결 시계를 분리한다.

각 batch는 **불변 batch_id**와 측정시각 window_us, n/raw_n/raw_dropped, 키별 통계·원본·해당 묶음의 clock_model/선택 raw_clock_anchor를 갖는다. 같은 측정시각 1초 구간이 여러 batch로 나뉠 수 있다. 서버는 `(stream,session,batch_id)`로 중복 제거하고 window_us로 재집계한다. schema2 CSV의 `(stream,session,window_us)` 중복 키를 schema3에 적용하면 정상 후속 묶음을 잃는다.

서버는 요청의 모든 묶음을 영속 저장/영속 큐 수락한 뒤 **204**를 반환한다. 이미 수락한 중복에도204를 반환한다. 200/202/오류/응답 단절은 자료와 ID를 유지해 다시 보낸다. TCP ACK는 이 애플리케이션 확인을 대체하지 않는다.

raw는 다음17필드다. t_us/window_us/utc_us와64bit 누적 계수는 정밀도를 위해 십진 문자열이다. 미상 값·오차·UTC는 null이다.

```text
t_us,seq,quality,key,unit,east_m,north_m,up_m,ve_mps,vn_mps,vu_mps,value,time_uncert_us,nav_age_us,pos_error_m,utc_us,utc_uncert_us
```

seq는 키별 중계 관측 순번이며 MAVLink 공용8bit헤더 순번이 아니다. quality는 원본 품질 비트와 위치오차 미상 등의 플래그다. 값·좌표는 재시도 중 재계산하지 않는다. 최상위 clock_model/raw_clock_anchor는 첫 묶음 기준의 진단이다. 각 batch에도 마감 당시 모델·원시 anchor를 따로 보존한다. 이 진단으로 raw를 재계산하지 않으며, 각 raw의 UTC/오차는 해당 관측을 처리한 당시의 추정이다.

통계는 키별 Welford 평균·표본분산 M2/(n−1)·min/max, 상대시각 오차와 추정위치 오차 요약이다. n<2 분산은null, position_error_n0이면 위치오차 요약도null이다. 평균은 표본 가중이며 거리/시간 가중이나 센서 정확도 신뢰구간이 아니다. 여러 batch의 평균을 단순 평균하지 말고 n·평균·M2로 결합해야 한다.

큐는 미확인8묶음, 요청당최대4묶음, 원본최대512행/묶음이다. 원본초과분도 통계에는 포함하고 누락 수를 기록한다. 큐가 가득 차면 새 묶음을 버리고 전송 중 자료를 덮어쓰지 않는다. --summary는 원본을 생략해 위치별 원본 복원에 불충분하다.

수신·관측처리, HTTP 인코딩/송신, TUI는 분리되어 있다. Windows WebSocket은 고정 수신 버퍼 작업자를 추가로 쓴다. 포트 검색은 프레임마다 수행하지 않는다. 파서 O(bytes×log(known messages)), device최대32·pending512·nav32로 모든 큐를 제한한다. pending 정렬·보간은 고정 상한이 있으나 하드 WCET를 주장하지 않는다. `--sizes`는 관리하는 고정 수집/송신/입력/경로 저장공간 합이며 스택·OS·TLS·libcurl 내부 메모리는 제외한다.

## 빌드와 개발 검사

CMake3.21+, Ninja, C99 x64 컴파일러가 필요하다. Windows WinHTTP/bcrypt, Linux libcurl/pthread를 사용한다. 기본 MinSizeRel은 검사 OFF이며 Windows/Linux 각각 두 운용 파일을 만든다.

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

선택 CTest는 코어·시계·설정·MAVLink 검사를 실행한다. 구성 시 Python3가 있으면 transport·실제 router 통합 검사도 등록하며, 교차 빌드에서는 타깃을 실행하지 않는다. 추가 회귀는 다음과 같다(Linux는 실행 파일 .exe 없이 경로를 바꾼다).

```powershell
python src/tests/test_core.py --skip-build
python src/tests/test_sender.py
python src/tests/test_confirmation.py
python src/tests/test_monitor.py
python src/tests/test_sender_limits.py
python src/tests/test_mavlink_router.py
```

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

## 근거와 적용 한계

[MAVLink HEARTBEAT](https://mavlink.io/en/services/heartbeat.html), [패킷 직렬화/CRC](https://mavlink.io/en/guide/serialization.html), [common 메시지](https://mavlink.io/en/messages/common.html), [TIMESYNC](https://mavlink.io/en/services/timesync.html)를 따른다. TIMESYNC만으로UTC가생기지는않는다. CRC표는고정된공식 c_library_v2출처이며 생성물라이선스는 src/vendor/MAVLINK_COPYING.txt에보존했다.

[OGC SensorThings](https://docs.ogc.org/is/18-088/18-088.html)의 관측시각/결과시각 구분, [SensorML](https://docs.ogc.org/is/23-000/23-000.html)의 시공간 기준틀 구분을 설계에 참고했다. 이 출력이 해당 API와 호환된다는 뜻은 아니다. 통계나 보간으로 미관측 물리장이 유일하게 복원되지는 않는다.

실제 센서 지연·GPS정확도·지오이드·서명키 검증과 타컴포넌트 시계 정렬은 장치 통합에서 확인해야 한다. 원본을 디스크에 자동 저장하지 않아 전원 차단/장기장애에는 손실이 발생한다. [검토 기록](REVIEW.md)에 실제 검증 범위를 구분했다.
