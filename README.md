# mavlink_telemetry_data_router

이동 중인 센서에서 받은 **관측값·측정시각·위치·속도**를 모아 서버로 전달하는 프로그램입니다. Windows x64와 Linux x64에서 사용합니다. Intel과 AMD에는 같은 x64 실행 파일을 사용합니다.

예를 들어 100 Hz로 들어온 관측을 한 개씩 서버에 보내는 대신, **1초 구간의 원본 100개와 통계를 묶어** 전송합니다. 기본 설정은 원본을 보존하며, 서버 요청 시작 간격은 최소 1초입니다.

## 무엇을 실행하면 되나요?

두 프로그램 중 **하나만** 실행하세요. 둘 다 수신·통계 처리·서버 전송을 자체적으로 수행합니다.

| 실행 파일 이름 | 사용할 때 |
|---|---|
| `mavlink_telemetry_data_router_interactive` | 화면에서 수신량·전송 상태를 보고 설정을 편집할 때 |
| `mavlink_telemetry_data_router_headless` | 화면 없이 자동 운용하거나 다른 프로그램에서 실행할 때 |

Windows 파일에는 `.exe`가 붙습니다. **interactive를 쓰기 위해 headless를 따로 켤 필요가 없습니다.**

현재 입력은 MAVLink 해독기가 만든 **정규화 CSV**입니다. Pixhawk의 UART/UDP MAVLink 패킷을 직접 해독하는 기능과 최종 저장 서버는 포함하지 않습니다.

```text
센서/Pixhawk → MAVLink 해독·시각/좌표 정렬 → 이 프로그램 → HTTP/HTTPS 저장 서버
```

## 파일은 어디에 있나요?

소스를 받은 폴더에서 실행 파일은 다음 위치에 있습니다. 소스만 내려받았다면 아래 빌드 절차를 먼저 수행하세요.

```text
src/                             프로그램·개발 검사 소스
config/telemetry.ini.example     설정 예제
examples/example.csv             실행 확인용 가상 관측
build/
  windows-x64/
    mavlink_telemetry_data_router_headless.exe
    mavlink_telemetry_data_router_interactive.exe
    tests/                       개발 검사를 선택한 경우에만 생성
  linux-x64/
    mavlink_telemetry_data_router_headless
    mavlink_telemetry_data_router_interactive
    tests/                       개발 검사를 선택한 경우에만 생성
  packages/                      Windows ZIP, Linux TAR.GZ, 소스 패키지
```

Windows와 Linux용 운용 프로그램은 **각각 2개, 합계 4개**입니다. 실행 파일은 자신이 사용할 OS의 것으로 선택합니다. 배포 ZIP/TAR.GZ를 받았다면 두 프로그램은 압축 안의 `bin/`에 있습니다. 아래 실행 예제는 소스 폴더 기준이므로 배포본에서는 프로그램 경로를 `bin/`으로 바꾸고 설정·예제 경로도 실제 위치에 맞춥니다.

## 먼저 예제로 실행하기

아래 명령은 **저장소 루트 폴더**에서 실행합니다. 기본 설정의 `url=-`는 서버에 접속하지 않고 JSON을 출력하는 시험 모드입니다. 여기서는 JSON을 버리고 상태 화면만 봅니다.

Windows PowerShell:

```powershell
Copy-Item .\config\telemetry.ini.example .\telemetry.ini
Get-Content .\examples\example.csv | .\build\windows-x64\mavlink_telemetry_data_router_interactive.exe --config .\telemetry.ini 1>$null
```

Linux:

```sh
cp config/telemetry.ini.example telemetry.ini
./build/linux-x64/mavlink_telemetry_data_router_interactive --config ./telemetry.ini < examples/example.csv > /dev/null
```

예제 파일을 모두 읽으면 프로그램이 종료됩니다. 계속 갱신되는 화면과 설정 편집을 사용하려면 실제 해독기의 CSV 출력을 연결해야 합니다. 다음 `your_decoder`는 그 해독기 실행 명령으로 바꿉니다.

```sh
your_decoder | ./build/linux-x64/mavlink_telemetry_data_router_interactive --config ./telemetry.ini
```

화면 없이 실행하려면 같은 위치의 `_headless` 프로그램을 선택합니다.

## 실제 서버로 보내기

`telemetry.ini`에서 우선 다음 항목을 설정합니다.

| 항목 | 설정할 내용 |
|---|---|
| `url` | 서버의 수신 주소. 예: `https://YOUR_SERVER/ingest` |
| `stream` | 관측 장치나 데이터 흐름을 구분하는 이름 |
| `session` | Pixhawk 부팅 또는 관측 실행마다 새로 정하는 식별자 |
| `origin_lat`, `origin_lon`, `origin_alt` | 위치 좌표의 기준 원점. 위도·경도·타원체고 |
| `token_env` | 인증 토큰을 담을 환경변수 이름. 기본 `TB_TOKEN` |

인증 토큰 **값**은 설정 파일 대신 지정한 환경변수에 넣습니다. `send_interval_ms=1000`은 서버 요청 시작 간격이며 센서 수신 빈도를 제한하지 않습니다.

interactive 화면에서 `e`로 설정을 열고, 항목을 고쳐 Enter로 확인한 다음 `s`로 저장할 수 있습니다. **저장한 설정은 재시작 후 적용**됩니다. 미확인 자료가 남았다면 먼저 정상 전송을 마친 뒤 재시작하세요.

화면에서는 수신률, 거부·누락 수, 대기 큐, HTTP 결과, 서버 확인 수, GPS 시각 추정 상태를 확인할 수 있습니다. 전체 설정과 키보드 조작은 [상세 사용법](docs/REFERENCE.md#상태-모니터와-서버-설정)에 있습니다.

## 서버가 꺼져 있으면 어떻게 되나요?

입력이 계속 열려 있는 동안에는 관측 수신과 interactive 화면 갱신이 계속됩니다. 전송에 실패한 자료는 메모리에 보관하고 다시 시도합니다.

서버가 모든 배치를 확정 수락한 뒤 **HTTP 204**를 반환하면 해당 자료의 메모리 공간을 재사용합니다. 200·202 등 다른 응답이나 연결 실패는 확정 수락으로 처리하지 않습니다. 서버도 배치 식별자로 중복을 제거해야 합니다.

메모리는 무한하지 않습니다. 대기 큐는 최대 8개 구간이고, 가득 차면 새 구간을 버리며 손실을 표시합니다. 관측은 디스크에 자동 저장하지 않습니다. 입력이 끝난 뒤에도 전송 실패가 계속되면 미전송 수를 알리고 종료하므로, 남은 메모리 자료는 소실됩니다.

## 소스에서 빌드하기

CMake 3.21 이상과 Ninja, 대상 OS의 x64 C 컴파일러가 필요합니다. 일반 빌드는 운용 프로그램 2개만 만듭니다.

Windows는 LLVM-MinGW의 x64 Clang과 Ninja를 PATH에 둔 뒤 실행합니다.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
cmake --build --preset package-windows-x64
```

Linux는 회사 GitLab 등의 저장소 화면에서 복사한 clone URL을 사용합니다. 아래 자리표시자를 실제 주소로 바꿉니다.

```sh
sudo apt-get update
sudo apt-get install git cmake ninja-build build-essential libcurl4-openssl-dev
git clone YOUR_REPOSITORY_CLONE_URL mavlink_telemetry_data_router
cd mavlink_telemetry_data_router
cmake --preset linux-x64
cmake --build --preset linux-x64
cmake --build --preset package-linux-x64
sh install.sh
```

Linux 설치 기본 위치는 `$HOME/.local/bin`이며 `PREFIX=/원하는/경로 sh install.sh`로 바꿀 수 있습니다. Linux 실행에도 libcurl와 호환되는 시스템 라이브러리가 필요합니다.

Windows에서 Linux용 파일도 교차 컴파일할 수 있습니다. 절차·컴파일러 경로 설정·Debug 빌드는 [상세 빌드 안내](docs/REFERENCE.md#cmake-소스빌드패키지-트리)를 참고하세요.

## test_ 프로그램은 왜 있나요?

`test_`로 시작하는 파일은 **개발자가 변경 후 오류를 확인하는 검사 도구**입니다. 수집기나 서버가 아니며 운용 프로그램이 호출하지도 않습니다. 평소에 실행하거나 배포할 필요가 없습니다.

| 개발 검사 | 확인하는 것 |
|---|---|
| `test_telemetry` | 평균·분산, 관측 순서, 용량 초과, 위치 보간, JSON 생성 |
| `test_time_sync` | UTC 추정, 시계 드리프트, 큰 시각 값, 잘못된 동기화 입력 |
| `test_config` | 설정값 범위, 인증정보 숨김, 한글 경로, 저장 실패 시 기존 파일 보존 |

세 검사를 하나의 프로그램으로 합칠 수도 있습니다. 현재는 **어느 부분이 실패했는지 구분하기 쉽도록** 나눴습니다. 검사 코드는 운용 프로그램에 포함되지 않으므로, 분리된 검사 프로그램이 있어도 운용 중 CPU·메모리 사용이나 실행 파일 크기는 늘지 않습니다.

검토 결과 검사는 유지하고 **필요할 때만 빌드**하도록 했습니다. 기본값은 `BUILD_TESTING=OFF`이며, 검사를 켜면 결과를 `build/<OS>/tests/`에 따로 만듭니다. 배포 ZIP/TAR.GZ에는 운용 프로그램 2개와 문서·설정 예제 등이 들어가며 `test_` 실행 파일은 제외됩니다.

개발자가 Windows에서 검사하려면:

```powershell
cmake --preset windows-x64-tests
cmake --build --preset windows-x64-tests
ctest --preset windows-x64-tests
```

Linux에서는 위 이름의 `windows`를 `linux`로 바꿉니다. CTest가 세 검사기를 실행하고 결과를 모아 보여 줍니다. HTTP 재시도·서버 확인·동시 수신·interactive 화면은 별도 Python 통합 검사로 확인합니다.

검사 프리셋은 같은 플랫폼 빌드 폴더를 재사용합니다. 일반 프리셋으로 다시 구성하면 검사가 OFF로 돌아가지만, 이미 생성된 `tests/` 파일을 자동 삭제하지는 않습니다. 그 파일은 실행 중인 서비스가 아닙니다.

## 상세 자료와 검증 범위

- [기술 참조](docs/REFERENCE.md): 입력 CSV·키/단위 규칙, GPS/UTC·좌표, 통계·오차 추정, 서버 규약, 교차 빌드, 전체 검사 절차
- [검토·검증 기록](docs/REVIEW.md): 실제 시험 결과, 계산량·메모리, 알려진 한계

Windows x64는 실제 실행 시험을 통과했습니다. Linux x64 파일은 Windows에서 실제 교차 컴파일하고 ELF 형식·의존성을 확인했으며, Linux에서의 직접 실행은 아직 검증하지 않았습니다.
