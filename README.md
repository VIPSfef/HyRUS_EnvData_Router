# Telemetry Batch 0.2.1

고속 이동 센서의 관측을 보존하고, 1초 구간별 원본·통계를 묶어 HTTP/HTTPS로 보내는 C99 프로그램이다.
기본은 **원본 + 통계**이며 `--summary`는 원본을 생략한다. 수신 빈도와 HTTP 요청 빈도를 분리한다.

## 구성과 연결 범위

`MAVLink 해독기/시각·좌표 정렬 → 정규화 CSV 또는 C API → 고정 큐 → 송신 스레드 → 서버`.
실제 MAVLink wire 해독기, UART/UDP 입출력, 최종 저장 서버는 포함하지 않는다.
기존 해독기는 아래 CSV를 출력하거나 `tb_sample`을 채워 `telemetry.c/.h`에 연결한다.
임의 숫자 `key/unit/value`를 처리하므로 해독된 새 key 추가에 코어 소스 변경이 필요 없다.
새 MAVLink wire 메시지 자체에는 송수신 측 dialect 정의와 해독 라이브러리 업데이트가 필요하다.
문자열·배열 값, 센서 교정, 좌표 변환, 순서 재정렬은 상위 어댑터의 책임이다.

## 빌드·설치·게시

Windows AMD64는 WinHTTP, Linux는 libcurl와 pthread를 사용한다. AMD/Intel x86-64에서 같은 아키텍처다.
Linux 실행 환경은 현재 없어 현장 실행은 미검증이다. GitHub/GitLab CI 파일은 준비했으며 원격 실행은 미수행이다.

```sh
sudo apt-get install git cmake build-essential libcurl4-openssl-dev python3
git clone YOUR_REPOSITORY_CLONE_URL telemetry-batch
cd telemetry-batch
sh install.sh
```

기본 설치는 `$HOME/.local/bin/telemetry_send`, 변경은 `PREFIX=/원하는/경로 sh install.sh`다.
실행 시 PATH 또는 절대 경로를 사용한다. Linux는 OS의 libcurl/TLS 라이브러리를 동적 사용한다.
이 폴더의 내용을 저장소 루트로 사용한다. 로컬 `main` 초기 커밋이 있으며 실제 remote URL은 아직 없다.
회사 GitLab·`prj.uaslaboratory.synology.me`에 접속/게시하지 않았다. 웹 호스트만으로 clone URL을 추측하지 않는다.
GitLab 프로젝트 화면에서 복사한 clone URL로 아래 자리표시자를 바꾼 뒤 사용자가 게시할 수 있다.

```sh
git remote add origin YOUR_GITLAB_CLONE_URL
git push -u origin main
```

`.github/workflows/`와 `.gitlab-ci.yml`에 빌드·테스트·설치·패키징 절차가 있다.
로컬 검증은 다음과 같다. Windows 다중 구성 빌드에는 `--config MinSizeRel`을 추가한다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
python test_core.py --cc clang --bench
python test_sender.py ./build/telemetry_send
cmake --install build --prefix ./stage
cmake --build build --target package
```

## 실행과 관측 입력

`telemetry_send URL|- STREAM SESSION ORIGIN_LAT ORIGIN_LON ELLIPSOID_ALT [--summary]`
Windows 예: `Get-Content .\example.csv | .\telemetry_send.exe - env1 trial-unique-001 37 127 100`.
Linux 예: `telemetry_send https://SERVER/ingest env1 trial-unique-002 37 127 100 < example.csv`.
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
`example.csv`는 boot0 UTC anchor, 100m/s 이동 중 두 key의 10Hz 관측 세 번, W=2초, 다음 구간의 새 key를 보여 준다.

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
이는 MCU 예측값이 아니다. 실제 실행파일/전체 고정 RAM/검증 범위는 [REVIEW.md](REVIEW.md), 해당 빌드는 `telemetry_send --sizes`로 확인한다.

## 설계 근거

[OGC SensorThings](https://docs.ogc.org/is/18-088/18-088.html)는 관측 phenomenonTime과 결과 생성 resultTime을 구분한다.
[OGC SensorML](https://docs.ogc.org/is/23-000/23-000.html)은 시간·공간 기준틀과 시각이 붙은 위치를 명시한다.
[MAVLink TIMESYNC](https://mavlink.io/en/services/timesync.html)는 반복 교환/RTT 기반 시계차 추정과 필터링을 설명한다.
[WMO 관측망 품질 지침](https://etrp.wmo.int/pluginfile.php/90334/mod_folder/content/0/INFCOM-3-RWCS-TECHNICAL-GUIDELINES-approved_updates.pdf)은 관측시각과 데이터베이스 수신시각을 구분해 지연을 평가한다.

## 2026-10-19: 서버 확정 수락과 메모리 반환

서버는 요청의 모든 배치를 영속 저장 또는 영속 큐에 확정 수락한 다음에만 HTTP 204를 반환한다. 이 프로그램은 204에만 해당 배치를 큐에서 제거하고 고정 버퍼를 재사용한다. 200/201/202 등 다른 상태나 전송 오류에는 자료와 배치 ID를 유지하여 재시도한다. HTTP 상태 코드 자체가 저장을 강제하지는 않으므로 이 규칙은 서버와 중계기의 애플리케이션 계약이다. TCP ACK는 바이트 전달 확인이며 이 확정 수락 응답을 대체하지 않는다. 서버는 (stream,session,window_us)로 중복을 제거하고 이미 확정 수락한 중복 요청에도 204로 답해야 한다. 큐에서 제거해도 버퍼 바이트를 즉시 0으로 지우지는 않는다. 다음 자료를 받을 때 같은 공간을 재사용하므로 메모리는 누적되지 않는다.

소스와 실행 파일의 작업 위치: C:\Users\pjs20\OneDrive\문서\New project 3\mavlink-server-transfer. 원격 Git 서버는 설정하지 않았다.
