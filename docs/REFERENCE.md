# 기술 참조 — mavlink_telemetry_data_router

처음 사용하는 경우 [README](../README.md)부터 읽는다. 이 문서의 명령은 저장소 루트에서 실행한다. 설정 전체 항목, 입력 CSV, 시각·좌표·통계 규약, 교차 빌드와 서버 구현자를 위한 상세 자료다.

고속 이동 센서의 관측을 보존하고, 1초 구간별 원본·통계를 묶어 HTTP/HTTPS로 보내는 C99 프로그램이다.
기본은 **원본 + 통계**이며 `--summary`는 원본을 생략한다. 수신 빈도와 HTTP 요청 빈도를 분리한다.

## 구성과 연결 범위

`MAVLink 해독기/시각·좌표 정렬 → 정규화 CSV 또는 C API → 고정 큐 → 송신 스레드 → 서버`.
실제 MAVLink wire 해독기, UART/UDP 입출력, 최종 저장 서버는 포함하지 않는다.
기존 해독기는 아래 CSV를 출력하거나 `tb_sample`을 채워 `src/telemetry.c/.h`에 연결한다.
임의 숫자 `key/unit/value`를 처리하므로 해독된 새 key 추가에 코어 소스 변경이 필요 없다.
새 MAVLink wire 메시지 자체에는 송수신 측 dialect 정의와 해독 라이브러리 업데이트가 필요하다.
문자열·배열 값, 센서 교정, 좌표 변환, 순서 재정렬은 상위 어댑터의 책임이다.

## 상태 모니터와 서버 설정

`mavlink_telemetry_data_router_headless`는 헤드리스 수집·중계기이고, `mavlink_telemetry_data_router_interactive`는 같은 수집·중계 기능에 TUI를 기본으로 켠 실행 파일이다. `mavlink_telemetry_data_router_headless --interactive`도 같은 기능을 제공한다.
모니터 자체가 수집·중계 프로그램이므로 별도 `mavlink_telemetry_data_router_headless` 프로세스를 실행하지 않는다. 둘 중 하나만 실행한다.
최종 수신 서버가 꺼져 있어도 모니터와 입력 수신은 계속 동작하고 재시도한다. 미확인 자료는 고정 RAM 큐에 남으며 큐가 가득 차면 새 구간을 버리고 손실을 표시한다.
관측 입력은 계속 표준입력으로 받고 화면과 진단은 표준오류로 출력한다. 입력은 MAVLink 해독기에서 정규화한 CSV다.
화면은 기본1Hz이며, 입력/송신/화면이 각각 다른 스레드에서 동작한다. 관측은 메모리에만 유지하고 설정만 파일로 저장한다.

| 진단 항목 | 판단에 쓰는 의미 |
|---|---|
| 전체·키별 observations/s, 최신 값·순번 | 센서/해독기 입력 중단, 특정 key 갱신 중단 |
| accepted/rejected, 순번 gaps, raw/queue 손실 | 입력 규칙 위반과 메모리 용량 초과를 구분 |
| 큐 사용량·최장 대기시간·전송 중 구간 수 | 서버 장애·지연으로 미확인 자료가 쌓이는지 |
| HTTP 상태·오류 코드, 204 ACK 수·재시도·응답시간 | TCP 전달과 서버의 확정 수락을 구분 |
| 인코딩 시간·요청 시도 바이트 | 직렬화 비용과 재전송 부하 확인(실제 선로 바이트가 아님) |
| boot 시각·watermark·열린 구간 | 입력은 조용하지만 구간 마감 선언을 기다리는 상태 확인 |
| ENU 위치·속도, 상대 시각 불확실도·항법 age·위치오차 추정 | 이동 관측의 시공간 품질 확인 |
| UTC ESTIMATED/UNKNOWN/STALE, anchor 수·age·드리프트·잔차 | 시계 모델의 가용성과 오래됨 확인 |
| ACTIVE 설정·token 환경변수 set/missing | 현재 적용된 서버·전송 조건과 인증 준비 확인 |

`SENDING / RETRY / IDLE / WAIT_WATERMARK` 등 상태를 표시한다. 마지막 상태/오류 코드는 마지막 완료 요청의 결과이며 전송 중에는 과거 결과다.
ACK 수는 확인된 **요청 수**다(한 요청에 여러 구간 가능). `DRY`의 `dry_releases`는 화면 출력 성공이며 서버 ACK가 아니다.
입력 bytes/lines/idle에는 주석·제어 행도 포함한다. 관측률과 key별 n에는 받아들인 관측만 포함하고 NaN도 한 관측으로 센다.
GPS fix·원본 MAVLink CRC·wire 패킷 손실률은 현재 입력 경계에서 알 수 없어 `n/a`다. 관측 순번 gap을 MAVLink 헤더 손실률로 해석하지 않는다.
UTC/위치오차는 추정 지표이며 시계 모델이 실제 센서 지연·GPS 품질을 자동 검증하지 않는다. STALE 경고는 화면 진단이고 저장된 과거 관측의 UTC를 바꾸지 않는다.

Windows PowerShell에서 첫 예제를 실행한다. `-`는 외부 전송 없이 JSON을 내보내는 시험 모드다.

```powershell
Copy-Item .\config\telemetry.ini.example .\telemetry.ini
Get-Content .\examples\example.csv | .\build\windows-x64\mavlink_telemetry_data_router_interactive.exe --config .\telemetry.ini 1>$null
```

실제 운용은 MAVLink 해독기의 CSV 출력을 파이프로 연결한다. 짧은 예제 파일은 EOF 직후 종료하므로, 계속 갱신되는 화면/설정 편집은 실행 중인 입력 소스에서 사용한다.
Linux 예: `your_decoder | mavlink_telemetry_data_router_interactive --config ./telemetry.ini`.
실제 서버 URL로 운용할 때 JSON 표준출력은 비어 있다. 시험 모드에서 JSON도 터미널에 출력하면 TUI와 섞이므로 `/dev/null` 또는 PowerShell `$null`로 보낸다.
관측 저장을 원하지 않으면 JSON 출력을 파일로 리디렉션하지 않는다.

- `e`: 설정 화면. 위/아래 또는 번호+Enter로 필드를 선택하고 편집한다.
- `Ctrl+U`: 필드 지우기, Backspace: 한 글자 지우기, Enter: 검증, Esc: 편집 취소.
- `s`: 검증된 설정을 임시 파일에 완성한 뒤 원자적으로 교체한다. 실패 시 기존 설정을 보존한다.
- Esc 또는 `q`: 설정 화면에서 돌아온다. 저장하지 않은 변경은 버린다. `q`는 수집기를 종료하지 않는다.

**저장된 변경은 재시작 후 적용**한다. 실행 중 ACTIVE 서버/stream/session/좌표 원점은 고정하므로 미확인 배치가 다른 서버/식별자로 이동하지 않는다.
재시작은 입력 소스를 정상 종료해 큐를 확인받은 후 수행한다. 실패 종료의 pending 자료는 RAM에만 있으므로 프로세스 종료와 함께 사라진다.
세션은 Pixhawk boot/관측 원점을 식별하도록 설정하고, boot/원점이 바뀌면 새 session을 지정한다.
키보드는 Windows `CONIN$` 또는 Linux `/dev/tty`에서 읽는다. CSV stdin 자체가 터미널이면 편집을 비활성화해 같은 입력을 가로채지 않는다.
표준오류가 리디렉션되면 ANSI 제어문자 없이 1Hz 상태를 출력하고 편집은 비활성화된다. 터미널에서는 위/아래로 key 목록을 스크롤한다.

설정은 UTF-8 `key=value`다. Windows 한글 경로를 지원하며 알 수 없는 키·중복·범위 밖 값·잘린 행은 시작 전에 거부한다.

| 설정 | 기본값 / 범위 |
|---|---|
| url | 필수. `https://HOST/path`, `http://HOST/path`, 시험용 `-`; URL userinfo 금지 |
| stream, session | 필수. ASCII 영숫자/`_-.`, 1–64자 |
| token_env | `TB_TOKEN`. 토큰 **값** 대신 환경변수 이름 |
| origin_lat/lon/alt | 기본0. WGS84 deg/deg/타원체고 m; 실제 관측 원점으로 설정 |
| send_interval_ms | 1000, 1000–60000; 재시도 포함 요청 시작 간격 |
| connect_timeout_ms / io_timeout_ms | 각각1000, 100–60000 |
| max_windows | 4, 1–컴파일 한도4; 요청당 최대 구간 수 |
| refresh_ms | 1000, 250–10000; 터미널 화면 갱신 |
| idle_warn_ms / ack_warn_ms | 2000 / 5000, 250–3600000; 화면 경고 기준 |
| summary | 0. 1이면 전송 원본을 생략하고 통계만 전송 |

Windows I/O timeout은 단계별 제한이다. Linux 총 요청 timeout은 `connect_timeout_ms + 2*io_timeout_ms`다. 동일한 전체 요청 최악시간을 보장하는 설정이 아니다.
토큰은 OS 환경에서 제공한다. 화면에는 환경변수 이름과 set/missing만 보이고 URL query는 숨긴다. 설정 파일에는 query 문자열이 그대로 저장되므로 인증정보는 query 대신 토큰 환경변수로 관리한다.
모드 선택은 `--interactive` / `--headless`를 사용한다. 이전 `--monitor` / `--no-monitor` 옵션도 호환용으로 허용한다. 기존 위치 인자 실행법과 `--summary`, `--sizes`는 유지하며 `--headless`로 화면을 끌 수 있다.

## CMake 소스·빌드·패키지 트리

모든 프로그램·테스트 소스는 `src/`에, 생성 파일과 네 실행 파일은 `build/`에 둔다.
기본 빌드는 **Windows x64 수집기/모니터와 Linux x64 수집기/모니터**다. Intel/AMD x64에 같은 실행 파일을 사용한다.
Debug는 별도 선택 사항이며 기본 네 실행 파일을 덮어쓰지 않는다.

```text
mavlink-server-transfer/
├── CMakeLists.txt
├── CMakePresets.json           # 공유 빌드·테스트·패키지 설정
├── cmake/                     # 아키텍처 검증·툴체인 설정
├── src/
│   ├── CMakeLists.txt
│   ├── sender.c
│   ├── telemetry.c / telemetry.h
│   ├── time_sync.c / time_sync.h
│   ├── config.c / config.h
│   ├── monitor.c / monitor.h
│   └── tests/                 # C·Python 검증 소스
├── config/telemetry.ini.example
├── examples/example.csv
├── docs/REVIEW.md
└── build/
    ├── windows-x64/
    │   ├── mavlink_telemetry_data_router_headless.exe
    │   ├── mavlink_telemetry_data_router_interactive.exe
    │   └── tests/              # 개발 검사 활성화 시 생성
    ├── linux-x64/
    │   ├── mavlink_telemetry_data_router_headless
    │   ├── mavlink_telemetry_data_router_interactive
    │   └── tests/              # 개발 검사 활성화 시 생성
    ├── packages/              # CPack ZIP/TGZ, 소스 패키지
    └── stage/                 # 설치 트리
```

`build/`와 개인별 `CMakeUserPresets.json`은 Git에서 제외한다. 공유 설정은 `CMakePresets.json`으로 관리한다.
빌드·테스트 결과와 플랫폼별 실제 실행 범위는 [검증 기록](REVIEW.md)을 확인한다.
Windows 검증을 Linux 검증으로 간주하지 않는다. 한 OS의 native preset만 실행하면 해당 OS의 두 파일이 생성된다.
나머지 OS에서도 같은 소스를 빌드하거나, 대상 라이브러리를 갖춘 명시적 교차 툴체인을 사용해야 한다.

### 공유 preset

CMake 3.21 이상과 Ninja를 사용한다. 기본 최적화는 실행 파일 크기를 줄이는 `MinSizeRel`이다.

| 목적 | Windows x64 | Linux x64 |
|---|---|---|
| 구성 | `cmake --preset windows-x64` | `cmake --preset linux-x64` |
| 빌드 | `cmake --build --preset windows-x64` | `cmake --build --preset linux-x64` |
| 개발 검사 구성·빌드·실행 | 아래 선택 검사 절차 | 아래 선택 검사 절차 |
| 설치 트리 | `cmake --install build/windows-x64` | `cmake --install build/linux-x64` |
| 패키지 | `cmake --build --preset package-windows-x64` | `cmake --build --preset package-linux-x64` |
| Debug 구성·빌드 | `windows-x64-debug` | `linux-x64-debug` |

일반·Debug 빌드는 `BUILD_TESTING=OFF`다. `windows-x64-tests`와 `linux-x64-tests`만 개발 검사를 활성화하며 기존 플랫폼 빌드 폴더를 재사용한다. 검사 실행 파일은 그 안의 `tests/`에 모인다.

Debug는 위 명령의 preset 이름을 바꾸며 결과는 `build/windows-x64-debug/`, `build/linux-x64-debug/`에 생성한다.
CPack 패키지는 `build/packages/`에 모으며 관측 자료는 패키지나 디스크에 저장하지 않는다.
설치 기본 위치는 `build/stage/<platform>/`이다. 실제 시스템 설치는 `cmake --install build/linux-x64 --prefix "$HOME/.local"`처럼 명시한다.

### Windows x64

LLVM-MinGW의 x64 `clang`과 Ninja를 PATH에 둔 환경에서 실행한다.
MSVC 대상 Clang을 쓰면 x64 Visual Studio 개발 환경과 해당 Windows SDK가 필요하다.
같은 빌드 디렉터리에서 서로 다른 컴파일러의 캐시를 섞지 않는다.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
cmake --build --preset package-windows-x64
Copy-Item .\config\telemetry.ini.example .\telemetry.ini
Get-Content .\examples\example.csv | .\build\windows-x64\mavlink_telemetry_data_router_interactive.exe --config .\telemetry.ini 1>$null
```

컴파일러 경로가 PATH에 없다면 첫 구성 때 `-DCMAKE_C_COMPILER=C:/YOUR_TOOLCHAIN/bin/clang.exe`로 지정한다.
머신별 설정을 보존할 때는 저장소 루트에 다음 `CMakeUserPresets.json`을 만들고 로컬 경로를 입력한다.
공유 preset에는 개인 절대 경로를 넣지 않는다. 로컬 preset도 기본 `build/windows-x64`를 사용한다.

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "windows-local",
      "inherits": "windows-x64",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "C:/YOUR_TOOLCHAIN/bin/clang.exe",
        "CMAKE_MAKE_PROGRAM": "C:/YOUR_NINJA/ninja.exe"
      }
    }
  ]
}
```

최초 구성은 `cmake --preset windows-local`, 이후 빌드·테스트는 공유 `windows-x64` preset을 사용한다.
`TB_TARGET_ARCH=x64`는 컴파일러의 **실제 출력 대상 검사 조건**이며 컴파일러 타깃을 바꾸는 옵션이 아니다.
32비트·다른 아키텍처이면 구성 단계에서 실패하고, `src/tests/test_arch.py`가 실제 PE/ELF 실행 파일도 검사한다.

### Linux x64 설치·빌드

Ubuntu/Debian x64의 개발 도구·libcurl가 필요하다. Windows는 WinHTTP, Linux는 libcurl와 pthread를 사용한다.
실제 프로젝트 화면에서 복사한 clone URL로 자리표시자를 바꾼다.

```sh
sudo apt-get update
sudo apt-get install git cmake ninja-build build-essential libcurl4-openssl-dev python3
git clone YOUR_REPOSITORY_CLONE_URL mavlink_telemetry_data_router
cd mavlink_telemetry_data_router
cmake --preset linux-x64
cmake --build --preset linux-x64
cmake --build --preset package-linux-x64
sh install.sh
```

`install.sh`는 기본 `build/linux-x64/`를 사용하고 `$HOME/.local/bin/mavlink_telemetry_data_router_headless`와 `mavlink_telemetry_data_router_interactive`에 설치한다.
`PREFIX=/원하는/경로 sh install.sh`로 설치 위치를 바꾸고 `BUILD_DIR=/원하는/빌드경로`로 빌드 위치를 바꾼다.
실행 환경에도 x64 동적 로더와 libcurl/TLS 라이브러리가 필요하며 정적 단일 바이너리 배포를 보장하지 않는다.
Ubuntu/PX4/Gazebo 의존성이 없는 중계기이며 특정 Ubuntu 버전의 실행 검증은 검증 기록과 구분한다.

### Windows에서 Linux x64도 교차 빌드

이 작업 폴더에는 Windows x64 두 파일과 Linux x64 두 파일을 실제로 생성했다. Windows 실행 시험은 통과했으며 Linux는 교차 빌드·ELF·의존성 검사까지 수행했다. Linux 런타임 실행은 검증하지 않았다.

Windows의 LLVM-MinGW Clang22/LLD, Ninja, Python3.9 이상을 PATH에 둔 뒤 아래를 실행한다. 일반 Linux native 빌드에는 이 절차가 필요 없다.

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

준비 스크립트는 공식 Ubuntu22.04 패키지를 HTTPS로 받고 SHA256를 확인한 뒤 `build/toolchains/`에만 추출한다. 패키지를 OS에 설치하거나 패키지 설치 스크립트를 실행하지 않는다. sysroot와 Python 추가 모듈은 배포물에 포함하지 않는다.
생성 ELF는 glibc2.34 이상과 OpenSSL 계열 `libcurl.so.4`를 필요로 한다. 교차 패키징은 TGZ 안의 두 실행 파일 권한도 `0755`로 보정한다.
교차 빌드 디렉터리를 Linux로 복사해 native CMake 캐시로 재사용하지 않는다. Linux에서 새로 빌드할 때는 소스만 가져와 native preset으로 구성한다.
### 개발자용 검사·패키지

핵심 검증은 이미 해당 플랫폼으로 빌드한 실행 파일에 수행한다. Linux 예시이며 Windows는 실행 파일에 `.exe`를 붙인다.

```sh
cmake --preset linux-x64-tests
cmake --build --preset linux-x64-tests
ctest --preset linux-x64-tests
python3 src/tests/test_arch.py build/linux-x64/mavlink_telemetry_data_router_headless --arch x64
python3 src/tests/test_arch.py build/linux-x64/mavlink_telemetry_data_router_interactive --arch x64
python3 src/tests/test_core.py --skip-build --executable ./build/linux-x64/tests/test_telemetry --bench
python3 src/tests/test_sender.py ./build/linux-x64/mavlink_telemetry_data_router_headless
python3 src/tests/test_confirmation.py ./build/linux-x64/mavlink_telemetry_data_router_headless
python3 src/tests/test_monitor.py ./build/linux-x64/mavlink_telemetry_data_router_headless --interactive-executable ./build/linux-x64/mavlink_telemetry_data_router_interactive
python3 src/tests/test_sender_limits.py ./build/linux-x64/mavlink_telemetry_data_router_headless
cmake --build --preset package-linux-x64
cmake --build build/linux-x64 --target package_source
```

GitHub/GitLab CI도 같은 preset·폴더를 사용하며 대상별 실행 파일 두 개와 패키지를 산출물로 보존한다.
CI 정의를 작성한 것과 CI를 실제 실행해 통과한 것은 구분한다. 자동 원격 게시나 서버 연결은 수행하지 않는다.
CMake preset 형식은 [CMake 3.21 공식 설명](https://cmake.org/cmake/help/v3.21/manual/cmake-presets.7.html)을 따른다.

### 원격 저장소 게시

이 폴더가 저장소 루트다. 회사 GitLab·`prj.uaslaboratory.synology.me`는 실제 clone URL을 설정해 게시한다.
웹 호스트만으로 clone URL을 추측하지 않는다. 저장소 화면에서 복사한 주소로 자리표시자를 바꾼다.

```sh
git remote add origin YOUR_GITLAB_CLONE_URL
git push -u origin main
```

## 실행과 관측 입력

`mavlink_telemetry_data_router_headless URL|- STREAM SESSION ORIGIN_LAT ORIGIN_LON ELLIPSOID_ALT [--summary]`
Windows 예: `Get-Content .\examples\example.csv | .\build\windows-x64\mavlink_telemetry_data_router_headless.exe - env1 trial-unique-001 37 127 100`.
Linux 예: `mavlink_telemetry_data_router_headless https://SERVER/ingest env1 trial-unique-002 37 127 100 < examples/example.csv`.
URL `-`는 JSON을 표준출력에 출력한다. `TB_TOKEN`이 있으면 Bearer 인증 헤더에 사용한다.
STREAM/SESSION은 ASCII 영숫자·`_ - .`의 1~64자다. 매 부팅·실행에 고유 SESSION을 사용한다.
원점은 실행 동안 고정된 WGS84 위도/경도(deg)·**타원체고(m)**이며 위치/속도는 ENU m/m·s⁻¹다.
PX4 NED→ENU는 축 `(y,x,-z)` 변환과 원점 일치가 필요하다. MSL 고도와 타원체고의 차이는 어댑터가 보정한다.
원점·시계가 재설정되면 새 SESSION/프로세스로 시작한다. 프로그램이 시계 종류를 수치 크기로 추측하지 않는다.

관측 한 줄은 다음 **15필드**다. `#` 주석/빈 줄은 무시하고 1,023자를 넘는 줄은 거부한다.

```text
t_us,seq,quality,key,unit,value,east_m,north_m,up_m,ve_mps,vn_mps,vu_mps,time_uncert_us,nav_age_us,pos_error_m
```

- `t_us`: 실제 관측시각을 Pixhawk boot microseconds(uint64)로 정렬한 값. PC/서버 수신시각이 아니다.
- `seq/quality`: uint32. seq는 key별 관측 순번, quality는 의미를 상위에서 정하는 보존용 비트마스크다.
- `key`: 최대31자 `[A-Za-z_][A-Za-z0-9_.-]*`; `unit`: 최대15자 `[A-Za-z0-9_./%*^+-]+`.
- `value`: double 숫자 또는 `nan`. 신규 key는 자동 등록하며 기본 최대16개/SESSION, 같은 key의 단위는 SESSION 동안 고정한다.
- 위치/속도는 그 관측시각의 값이다. `time_uncert_us`는 센서 관측과 항법의 **상대 시각 정렬오차 추정**, 기본 최대20,000µs다.
- `nav_age_us`는 정렬에 쓴 항법과 관측의 시간 거리이며 보간 시 양쪽 중 큰 거리, 기본 최대200,000µs다.
- `pos_error_m`는 시각 오차 항을 제외한 위치오차 추정이다. 음수/비유한 값은 거부한다.
- 위치·속도·유효 value의 절댓값은 ≤1e9다. 물리량별 정상 범위·교정·품질판정은 어댑터가 수행한다.

전체 입력 관측의 t는 비감소, 같은 key의 t와 seq는 각각 엄격히 증가해야 한다. 서로 다른 key는 같은 t를 가질 수 있다.
UDP 순서 역전은 입력 전에 정렬한다. 같은 측정값을 통신 주기마다 새 관측으로 재등록하지 않는다.
서로 다른 측정 주기의 key는 새 값이 생겼을 때만 행을 추가한다. 누락값은 JSON null이며 그 key의 통계 n에서 제외된다.
MAVLink 헤더 8비트 순번은 여러 메시지가 공유하므로 관측 seq로 쓰지 않는다. 시각 없는 메시지는 별도 측정시각 정보가 필요하다.

## UTC 추정과 구간 마감

제어 행은 `@clock,boot_us,utc_unix_us,gnss_uncert_us,rtt_us`와 `@watermark,W`다.
clock은 **동일 부팅의 동시 시각 대응점**이다. UTC는 Unix µs이며 GPS/TAI 시간은 상위에서 UTC로 변환한다.
GNSS·시각 포착 오차 및 SYSTEM_TIME boot-ms 양자화는 gnss_uncert_us에 포함하고 실제 교환 RTT를 제공한다.
TIMESYNC는 장치 간 상대 시계차/RTT 추정에 쓰며 UTC 기준 자체를 만들어 주지는 않는다.
최근16개 anchor에 중심화 OLS를 적용하고 기본 ±500ppm 필터·마지막 anchor 후 최대10초 외삽을 사용한다.
추정 UTC 오차는 max anchor uncertainty + ceil(max RTT/2) + max fit residual + nearest-anchor age × 500ppm + 수치 반올림 여유다.
이는 제공된 오차/드리프트 가정에 의존하는 **공학적 추정**이며 95% 신뢰구간이나 절대 정확도 보증이 아니다.
짧은 anchor 간격의 잡음은 엄격한 드리프트 필터에 거부될 수 있다. RTT 비대칭·GPS→UTC 변환·UTC step도 별도 고려한다.
anchor의 boot/UTC는 각각 증가해야 한다. 부팅/UTC step·불연속은 새 SESSION/모델 초기화가 필요하다.
anchor 부재·범위 밖·오래된 모델이어도 관측은 보존하고 utc_us/utc_uncert_us를 null로 보낸다. 과거 행은 소급 재보정하지 않는다.
UTC 절대오차는 상대 time_uncert_us와 별개다. 상대 정렬에 공통인 UTC 오차를 위치오차에 다시 더하지 않는다.

구간은 boot 시각의 `[floor(t/1e6)*1e6, +1초)`다. 다음 구간 관측, EOF, 또는 충분히 진행한 watermark가 마감한다.
watermark는 앞으로 `t<W` 관측이 오지 않는다는 상위 소스의 선언이다. 이후 `t<W`는 거부하고 `t==W`는 허용한다.
W는 비감소이며 마지막 관측의 t 이상이어야 한다. 현재 구간이 없어도 기억한다. 이미 마감한 구간은 다시 열지 않는다.
PC에서 입력이 잠시 없다는 이유로 자동 마감하지 않는다. 따라서 고정1초 지연이나 벽시계 정시 업로드를 보장하지 않는다.
`examples/example.csv`는 boot0 UTC anchor, 100m/s 이동 중 두 key의 10Hz 관측 세 번, W=2초, 다음 구간의 새 key를 보여 준다.

## 통계·보간·출력 스키마

key마다 Welford로 유효 n·평균·표본분산 M2/(n−1)·최소·최대를 누적한다. n=0 통계와 n<2 분산은 null이다.
시간 불확도의 평균/최대, `pos_error_m + ||v||*time_uncert_us/1e6`의 평균/최대도 유효 value만 집계한다.
평균은 표본 가중이며 시간/거리 가중이 아니다. 분산은 센서 정확도·공간 분해능·독립 표본의 신뢰구간이 아니다.
코어 `tb_align`은 같은 시계·SESSION의 양 끝 항법 사이 선형 보간만 수행하며 외삽/200ms 초과 간격을 거부한다.
위치오차는 끝점 오차의 선형 보간 + `A*Δt²/8`이다. 마지막 항은 실제 전 구간 가속도 크기가 A 이하일 때만 수학적 상한이다.
센서 응답/흡입관 지연을 모르면 자동 역보정하지 않는다. 100m/s에서 10Hz는 약10m, 100ms 시간오차도 약10m다.
원본은 관측 경로의 재집계·보간에 쓸 수 있지만 미관측 물리장을 유일하게 복원하지 못한다. summary는 위치별 원본 복원에 불충분하다.

JSON은 자체 `schema:2`이며 OGC SensorThings/SensorML API 호환을 주장하지 않는다.
최상위에 stream/session, 고정 ENU 원점, 시계 기준, field_order, 누적 loss, clock_model, batches를 포함한다.
clock_model은 송신 시점의 모델 진단이며, 각 행 UTC/오차는 수신 시 적용한 추정값을 보존한다.
각 raw 행의 **17필드**는 출력 field_order 그대로다(입력 CSV 순서와 다름).

```text
t_us,seq,quality,key,unit,east_m,north_m,up_m,ve_mps,vn_mps,vu_mps,value,time_uncert_us,nav_age_us,pos_error_m,utc_us,utc_uncert_us
```

t_us/window_us/known utc_us와 누적 loss는 64비트 정밀도를 위해 십진 문자열이다. UTC 미상은 마지막 두 필드 모두 null이다.
`tb_batch_prepare`가 raw를 (t_us,key,seq), 통계를 key로 정렬한다. 동일 관측 집합·key별 순서를 가진 비포화 배치는 바이트가 일치한다.
포화 시 먼저 도착한512행만 보존하므로 retained subset은 도착 순서에 의존한다. stats에는 초과 관측도 포함한다.

## 서버·손실·계산량

배치 식별키는 **(stream,session,window_us)**다. 서버는 요청 내 모든 배치를 영속 저장/영속 큐 수락한 뒤에만 204로 ACK한다.
서버는 식별키로 중복을 제거해야 한다. ACK 유실 재시도가 가능하며 HTTP exactly-once 전달을 보장하지 않는다.
송신 시작은 초당 최대1회, HTTP 요청 하나에 최대4구간, 완성 대기 큐8개다. 느린 요청 뒤에도 몰아서 시작하지 않는다.
기본 원시512관측/초는 100Hz × 4key = 400행/초를 수용한다. raw 초과는 raw_dropped와 통계에 반영한다.
큐 포화는 새 배치를 버리고 전송 중 자료를 보존한다. 전역 손실 카운터는 서로 겹칠 수 있으므로 합산해 손실률로 쓰지 않는다.
EOF 후 연속 전송3회 실패하면 종료2와 pending을 남긴다. 종료0=정상,1=설정/시작 오류,2=거부·손실·미전송/실행 오류다.
디스크 spool은 없다. 전원 종료 시 최대 대기8+진행1구간(보존 raw 최대4,608행) 및 그 통계가 소실될 수 있다.
장기 단절 무손실이 필요하면 상위 원본 기록이나 영속 spool을 추가해야 한다.

수신 추가는 O(16), 경계 복사·초기화는 O(512), 송신 스레드 heapsort는 O(N log N), JSON은 O(N)이다.
네트워크·정렬·JSON은 송신 스레드에서 수행한다. 초 경계 복사/초기화의 짧은 잠금 비용도 실시간 예산에 포함해야 한다.
CPU 예산은 `f_observation*C_add + f_window*C_seal + f_request*C_encode`로 나눠 산정한다. 요청 묶음은 원본 정보량을 줄이지 않는다.
검증 PC에서 sample136B/batch71,840B. 백만 정상 추가의 5회 중앙값은 약76ns/건(초기화 포함, 정렬·JSON·네트워크 제외)이었다.
이는 MCU 예측값이 아니다. 실제 실행파일/전체 고정 RAM/검증 범위는 [검증 기록](REVIEW.md), 해당 빌드는 `mavlink_telemetry_data_router_headless --sizes`로 확인한다.

## 설계 근거

[OGC SensorThings](https://docs.ogc.org/is/18-088/18-088.html)는 관측 phenomenonTime과 결과 생성 resultTime을 구분한다.
[OGC SensorML](https://docs.ogc.org/is/23-000/23-000.html)은 시간·공간 기준틀과 시각이 붙은 위치를 명시한다.
[MAVLink TIMESYNC](https://mavlink.io/en/services/timesync.html)는 반복 교환/RTT 기반 시계차 추정과 필터링을 설명한다.
[WMO 관측망 품질 지침](https://etrp.wmo.int/pluginfile.php/90334/mod_folder/content/0/INFCOM-3-RWCS-TECHNICAL-GUIDELINES-approved_updates.pdf)은 관측시각과 데이터베이스 수신시각을 구분해 지연을 평가한다.
