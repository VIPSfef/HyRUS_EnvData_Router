# Telemetry Batch 0.1.0

고속 이동 환경센서 수신기 뒤에 붙이는 작은 C99 수집·중계 프로그램.
샘플링은 유지하고 HTTP 요청 시작 간격만 최소 1초로 제한한다.
기본값은 **원본 + 통계**, `--summary`는 통계만 보낸다.

## 구성과 범위

```
기존 MAVLink 해독기 + 측정시각 기준 항법 정렬
    -> 정규화 CSV (표준입력)
    -> telemetry_send (C, 수집 + 전송 스레드)
    -> HTTP/HTTPS JSON POST (서버)
```

`telemetry.c/.h`는 OS에 독립적인 고정 메모리 코어다.
`sender.c`는 Windows WinHTTP 또는 Linux libcurl을 사용하는 실행 예제다.
MAVLink XML/센서 모델/실제 포트/서버 스키마가 정해지지 않았으므로 **MAVLink 바이트 해독기와 최종 저장 서버는 포함하지 않는다.**
기존 해독기가 CSV를 출력하면 바로 연결할 수 있다. 자체 C 수신기에 결합할 때에는 `tb_sample`을 채워 코어 API를 호출한다.
CSV는 입출력 어댑터이며 MAVLink 자체 패킷 형식이 아니다.

## Linux: GitHub에서 설치

이 폴더의 **내용을 저장소 루트**에 올린다. 실제 GitHub 주소는 아직 지정되지 않았다.
Ubuntu/Debian AMD64에서 필요한 패키지:

```sh
sudo apt-get install git cmake build-essential libcurl4-openssl-dev
git clone YOUR_REPOSITORY_URL telemetry-batch
cd telemetry-batch
sh install.sh
```

기본 설치 경로는 `$HOME/.local/bin/telemetry_send`다. PATH에 없으면 절대 경로로 실행한다.
관리자 권한은 의존성 설치에만 필요하다. `PREFIX=/원하는/경로 sh install.sh`로 설치 위치를 변경한다.
Linux는 운영체제 libcurl/TLS 라이브러리를 동적 사용한다. 정적 단일 바이너리 배포를 보장하지 않는다.
현재 기기에는 Linux 실행 환경이 없어 Linux 실행 검증은 미수행이다. 포함한 GitHub Actions가 Ubuntu AMD64 빌드·테스트·설치·패키징을 수행한다.

## Windows AMD64

동봉 `telemetry_send.exe`는 Windows x86-64/AMD64 실행 파일이다. AMD와 Intel의 x64 CPU에서 같은 아키텍처로 실행한다.
Windows 기본 HTTP 기능을 사용하므로 별도 libcurl 설치가 필요 없다.
콘솔 프로그램이며 자동 실행 서비스 등록은 포함하지 않는다.

```powershell
Get-Content .\example.csv | .\telemetry_send.exe - env1 trial-001 37 127 100
Get-Content .\example.csv | .\telemetry_send.exe http://127.0.0.1:8080/ingest env1 trial-002 37 127 100
```

첫 명령의 URL `-`는 서버 전송 없이 JSON을 표준출력으로 보여 준다.
두 번째 명령은 해당 주소에 JSON 수신 서버가 실행 중이어야 한다.
예시 SESSION은 재사용하지 말고 실제 실행마다 새로운 UUID/고유 문자열을 지정한다.

## 빌드와 검증

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
python test_sender.py ./build/telemetry_send
cmake --install build --prefix ./stage
cmake --build build --target package
```

Windows는 Visual Studio 또는 LLVM-MinGW가 설정된 개발 환경에서 동일하게 빌드한다.
다중 구성 빌드는 `--config MinSizeRel`을 추가하고 실행 파일 경로를 해당 구성 폴더로 바꾼다.
테스트용 Python만 필요하며 운영 프로그램에는 Python이 필요 없다.

독립 코어 검증/마이크로벤치:

```sh
python test_core.py --cc clang --bench
```

## 실행과 입력 계약

```
telemetry_send URL|- STREAM SESSION ORIGIN_LAT ORIGIN_LON ELLIPSOID_ALT [--summary]
```

- `STREAM`, `SESSION`: ASCII 영숫자 및 `_-.`, 1~64자. 단일 물리 센서·측정 순번 스트림을 한 프로세스에서 처리한다.
- `ORIGIN_LAT/LON`: 고정 ENU 원점의 WGS84 위도/경도(deg). 마지막 값은 WGS84 타원체고(m).
- `TB_TOKEN` 환경 변수가 있으면 `Authorization: Bearer ...`를 보낸다. 외부 전송에는 HTTPS 주소를 사용한다.
- 표준입력은 샘플당 한 줄. `#` 주석과 빈 줄은 무시한다. 1,023자를 넘는 입력 줄은 거부한다.
- 종료 코드는 0=모두 정상 전송, 1=설정/스레드 시작 오류, 2=거부·누락·미전송 중 하나 이상 발생이다.

필드 순서(`example.csv` 참고):

| 필드 | 계약 |
|---|---|
| `t_us` | 서버 수신시각이 아닌 **동기화된 실제 측정시각**, UNIX microseconds(uint64) |
| `seq` | 센서 전용 단조 증가 uint32 순번. 재부팅/랩어라운드 전에 새 SESSION·프로세스 시작 |
| `quality` | 상위 센서/해독기가 부여한 uint32 품질 플래그. 코어는 그대로 보존 |
| `east_m,north_m,up_m` | 해당 측정시각에 정렬된 고정 원점 ENU 위치(double) |
| `ve_mps,vn_mps,vu_mps` | 같은 시각 ENU 속도(float) |
| `temp_C,rh_pct,pm25_ug_m3,pm10_ug_m3` | 새 관측만 기록. 누락/미갱신 채널은 `nan` |
| `nav_age_us` | 정렬에 사용한 항법 관측의 측정시각과 센서 측정시각의 절대 차이(us). 보간 시 보수적으로 양 끝 중 큰 차이 |

반복 수신한 같은 센서 측정값을 새 관측처럼 넣지 않는다. 예를 들어 온도100Hz/PM1Hz이면 나머지 PM99개는 `nan`이다.
한 줄의 네 채널은 동일 측정시각을 공유한다. 채널별 측정시각이 다르면 별도 행/스트림으로 정규화한다.
동일·역순 시각/순번은 거부한다. UDP 순서 역전/다중 센서 재정렬은 상위 어댑터 책임이다.
`nav_age_us` 기본 한계는 200,000us이며 이는 정확도 보증이 아니다. 장치 속도와 허용 위치오차로 더 작게 정해야 한다.

값 범위는 유한 위치/속도 절댓값≤1e9, 온도≥-273.15°C, 습도0..100%, PM≥0이며 유효 센서값의 절댓값≤1e9다.
Inf는 거부한다. NaN 센서 채널은 개별 통계에서 제외하고 JSON `null`로 보존한다.

## MAVLink 연결 시 주의

공식 [TIMESYNC](https://mavlink.io/en/services/timesync.html)로 시계 차이를 추정하고, 센서 측정시각의 항법 이력을 정렬한다.
[GLOBAL_POSITION_INT](https://mavlink.io/en/messages/common.html#GLOBAL_POSITION_INT)의 위·경도는 degE7, 고도는 MSL mm, 속도는 NED cm/s다.
이 프로그램의 ENU m / m/s / 타원체고와 직접 같은 값이 아니다. NED -> ENU 축은 `(y,x,-z)`이며 MSL과 타원체고 사이 지오이드 보정도 일치해야 한다.
PX4 로컬 좌표의 원점 재설정이 발생하면 새 SESSION/원점을 사용한다.
MAVLink 헤더의 8비트 패킷 순번은 다른 메시지와 공유되므로 이 프로그램의 `seq`에 쓰지 않는다.
`HYGROMETER_SENSOR`에는 측정시각 필드가 없다. 고속 이동 정렬에는 측정시각을 담은 커스텀 메시지 또는 별도 동기화된 계측 정보가 필요하다.

## 통계와 공간 정보

각 1초 측정시각 구간마다 채널별 유효 개수, 평균, 표본분산 `M2/(n-1)`, 최솟값, 최댓값을 Welford 방식으로 누적한다.
빈 채널은 null, 관측1개 분산도 null이다. 분산은 센서 정확도나 독립 표본에 대한 신뢰구간이 아니다.
평균은 **표본 가중 평균**이며 시간가중·거리/공간가중 평균이 아니다.

기본 원본 묶음은 시간·위치·속도·순번·품질을 보존하므로 서버에서 경로 표시, 위치 구간별 재집계, 시공간 보간을 수행할 수 있다.
이는 수신 원본의 복원이며, 관측 사이의 실제 물리장 복원을 보장하지 않는다.
`--summary`는 원본을 생략하여 위치별 봉우리·발생 순서 복원이 불가능하므로 이동 관측의 기본값으로 쓰지 않는다.

속도 `v`, 측정 주파수 `f`이면 샘플 간 이동거리는 약 `v/f`, 시간정렬 오차 `delta_t`에 따른 위치오차는 약 `|v|*delta_t`다.
100m/s 이동에서는 10Hz=10m, 100Hz=1m이며, 100ms 정렬오차만으로 약10m가 이동한다.
센서 응답지연/흡입관 지연 `tau`도 약 `|v|*tau`의 공간 오프셋을 만든다. 센서 모델·교정 없이 자동 역복원하지 않는다.
[SPS30](https://sensirion.com/resource/datasheet/sps30)처럼 자체 측정 간격이 약1초인 센서는 10Hz로 통신해도 독립적인 10Hz 공간 관측이 아니다.

## 서버 계약 및 장애

POST JSON 최상위에는 schema, stream, session, 좌표 원점, 필드 순서, 누적 손실계수, `batches` 배열이 있다.
배치 식별키는 **(stream, session, window_us)**. `window_us`/`t_us`/누적 손실계수는 64비트 정밀도를 위해 십진 문자열이다.
서버는 배치 식별키로 중복을 제거하고, **요청 내 모든 배치를 영속 저장/영속 큐 수락한 뒤 2xx**를 반환해야 한다.
부분 저장 실패를 2xx로 응답하면 안 된다. ACK 유실로 재전송될 수 있으므로 exactly-once HTTP 전달은 보장하지 않는다.

연결 복구 시 한 요청에 최대4초 배치를 묶어 backlog를 줄인다. 평상시와 재시도 모두 요청 시작 간격은 최소1초다.
입력이 없으면 마지막 입력 후1초 이상 지난 부분 배치도 마감하며, 추가 샘플 없이도 전송된다. 늦게 도착한 이미 마감된 구간 샘플은 거부한다.
측정시각 구간 끝에 다음 샘플이 와야 일반 구간이 마감되므로 고정1초 지연/정확한 벽시계 정시 업로드를 보장하지 않는다.

기본값은 원본128개/측정1초, 완성 배치8개 대기다. 100Hz를 수용하되 무제한 주파수/무제한 통신 장애를 수용하지 않는다.
원본 초과는 **통계에 포함**하고 `raw_dropped`를 늘린다. 큐 초과는 **새 배치를 버리고**, 전송 중 자료는 덮지 않는다.
누락/거부는 JSON 및 종료 진단에 남는다. 카운터는 중복될 수 있으므로 합산해 손실률로 쓰지 않는다.
EOF 이후 전송3회 연속 실패 시 종료2와 pending개수를 남긴다. **디스크 spool은 없으며 종료/전원 차단 시 메모리의 미전송 자료는 사라진다.**
장시간 단절에서도 무손실이 필요하면 상위 수신기의 원본 파일 기록 또는 별도 영속 spool을 결합해야 한다.

## 크기와 계산량

- 검증한 Windows AMD64 MinSize 빌드: 실행 파일23,040B(빌드 설정에 따라 달라짐).
- 코어: 샘플72B, 배치9,416B. 기본 송신 예제의 고정 데이터302,024B(약295KiB); OS/스레드 스택/HTTP·TLS 메모리 별도.
- `telemetry_send --sizes`로 실제 빌드의 고정 크기를 확인한다.
- 코어 추가는 O(4), 메모리 할당/네트워크 없음. 초 경계에는 배치복사+초기화 O(N), 약18KiB 메모리 작업이 짧은 잠금 안에서 발생한다.
- JSON은 O(B*N*4), 네트워크와 함께 전용 송신 스레드에서 실행. 수집 경로에서 HTTP를 기다리지 않는다.
- 예상 CPU시간/초는 `f_sample*C_add + f_window*C_seal + f_request*C_encode`로 분리해야 한다. 10Hz만으로 서버 부하가 크다고 단정할 수 없다.
- 원본 묶음은 요청 횟수·헤더/서버 작업 부담을 줄이며 원본 정보량 자체를 줄이지 않는다. 압축/바이너리 인코딩은 미포함.
- 작은 장치용은 모든 소스에 같은 `-DTB_CAPACITY=16 -DTB_QUEUE_CAP=4 -DTB_SEND_WINDOWS=2`를 적용할 수 있다(16samples/s 계약으로 축소). `TB_MAX_NAV_AGE_US`도 변경 가능.

독립 리뷰와 실제 검증 결과는 [REVIEW.md](REVIEW.md)를 참고한다.
