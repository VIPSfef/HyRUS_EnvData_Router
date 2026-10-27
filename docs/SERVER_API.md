# 서버 수신 API — mavlink_telemetry_data_router 0.11.0

이 문서는 수집 중계기 0.11.0이 보내는 데이터를 서버에서 받아 저장하는 방법을 설명합니다. 2026-10-26의 C 송신 코드를 기준으로 작성했으며, 프로그램 버전과 JSON의 `schema` 버전은 별개입니다. 운영 서버와 데이터베이스는 이 저장소의 구현 범위가 아니며, 여기서 설명하는 저장·중복 처리 기능은 서버 개발자가 구현해야 합니다.

구현 주체는 [HyRUS_EnvData_Router](https://github.com/VIPSfef/HyRUS_EnvData_Router)입니다. 이 저장소는 받은 MAVLink를 분석해 전송하며, I²C·PX4 센서 발행은 별도 `HyRUS_EnvData_Px4`에서 담당합니다. 그 앞단 규약은 [PX4 → Router 입력 규약](PX4_MAVLINK_CONTRACT.md)에 있습니다. 번들 시험 서버는 수신·표시를 검증하는 도구로, 운영 서버 구현을 대신하지 않습니다.

서버를 구현할 때는 다음 파일을 함께 참고하세요.

- [OpenAPI 3.1](api/openapi.json): HTTP 엔드포인트 계약.
- [JSON Schema 2020-12](api/ingestion.schema.json): 본문 구조 검증.
- [UDP 백업 규약](UDP_BACKUP.md): 같은 센서 JSON을 HTTP와 독립적으로 받는 조각 전송 형식. 기본 UDP 포트는 HTTP TCP 포트+1.
- [합성 예제와 검증 안내](api/README.md): 실제 관측을 포함하지 않는 요청 예제.
- [Git·OneDrive 공유 절차](COLLABORATION.md).
- [전체 기술 참조](REFERENCE.md), [검증 기록](REVIEW.md).

## 1. 서버에서 구현할 처리 흐름

### 선택 사항: 주기 상태 보고

`status_interval_ms=10000`이면 MAVLink 수신 모드에서 같은 URL로 새 관측 유무와 관계없이 약10초마다 `mavlink_router_status_v1`을 보냅니다. 기본값0은 비활성화이며 기존 API에는 새 형식을 자동으로 보내지 않습니다. 상태 보고는 관측용 큐와 별개이고 `sample_count=0`입니다. 이 절을 제외한 아래의 저장·재시도 계약은 측정 요청에 적용됩니다.

본문은 `{"schema":"mavlink_router_status_v1","stream":"...","records":[...]}`이며 현재 송신기는 요청마다 최신 상태 한 건을 담습니다. 64비트 시간·카운터·순번은 정밀도 손실을 피하기 위해 십진 문자열입니다.

| 필드 | 의미 |
|---|---|
| `seq`, `generation`, `session` | 보고 시도 순번, 입력 세대, 실행·소스 세션. 순번은 같은 프로세스의 설정 재시작에서도 증가 |
| `reported_monotonic_ms` | 호스트 단조 시계의 보고 스냅샷 시각. UTC나 센서 측정시각이 아님 |
| `run_elapsed_ms` | 프로세스 시작 이후 누적 실행 시간. 기기 수신 경과 시간과 별개 |
| `source.sysid/compid`, `endpoint`, `serial_baud`, `state` | 선택 기체와 현재 입력/탐색 상태. 재탐색 중 endpoint는 검사 중인 후보이며 기체 ID는 마지막 선택 기체를 유지 |
| `source.connected` | 선택 기체의 하트비트가 유효하고 입력 상태가 LOCKED일 때만 true. 단순 COM 열림·모뎀/GCS 수신으로 true가 되지 않음 |
| `source.heartbeat`, `hb_age_ms` | WAIT/LIVE/LOST와 선택 기체의 마지막 하트비트 수신 경과 ms. 미수신 시 null |
| `source.data`, `data_age_ms` | WAIT/LIVE/STALE와 해당 기체의 하트비트 이외 데이터 수신 경과 ms |
| `weather.temperature/humidity` | 마지막 값(value), 단위(K 또는 비율1), UNKNOWN/FRESH/STALE, 자체 `rx_age_ms`, 해당 키의 누적 `observations`. 새 표본이 없으면 관측 수는 늘지 않음 |
| `position` | 마지막 수신 GPS/ESTGPS 좌표와 `fix`, `state`(LIVE/STALE/INVALID), 자체 `rx_age_ms`, `stale_after_ms`. `lat_deg`, `lon_deg`, `alt_msl_m` 단위는 도·도·m. 미수신은 null이고 이전 클라이언트는 필드를 생략 가능 |
| `packets` | 선택 기체의 연결 세션 수신 수, 추정 누락 수, 중복 수. 확정 무선 손실량이 아님 |
| `devices[]` | 최대32개 sys.comp별 하트비트·데이터 상태와 경과 시간, 마지막 프레임 `rx_age_ms` |

연결과 하트비트·데이터 만료는 `link_timeout_ms`를 사용하고 온습도 FRESH/STALE는 `idle_warn_ms`를 사용합니다. 값이 없으면 `value=null`, `rx_age_ms=null`, `observations="0"`입니다. 센서 값이 오래됐다는 이유만으로 기체 연결이 끊겼다고 판단하지 않습니다. 완전 단절 후에는 마지막 기체와 장치별 수신 시각을 보존하고 재탐색하며, 새 입력을 선택하면 새 세션의 정보로 교체합니다. 다른 기체의 과거 값을 새 기체 값으로 넘기지 않습니다.

`position`은 센서 관측에 위치를 결합할 수 있는 짧은 정렬 허용 시간과 별개로 마지막 좌표를 보존합니다. STALE/INVALID 좌표를 현재 유효한 위치나 새 GPS 표본으로 집계하지 마세요. 서버 화면의 좌표 age는 받은 보고의 `rx_age_ms`에 서버에서 그 보고를 받은 뒤 지난 시간을 더한 값입니다. 서버의 `last_rx`는 마지막 수락 레코드 이후 시간이며 센서 age·GPS age·실행 시간과 다릅니다.

상태 요청의204는 상태 보고 수락만 확인하며 측정 큐를 해제하지 않습니다. 실패한 상태를 쌓거나 같은 스냅샷을 재시도하지 않고 다음 주기에 최신 상태를 보냅니다. 측정 전송과 별도 기한을 사용하므로 `send_interval_ms=60000`이어도10초 상태 보고를 시도하지만, HTTP 작업자와 네트워크는 공유하므로 느린 요청·timeout은 실제 전달을 지연시킬 수 있습니다. 종료·설정 적용을 위한 배출 중에는 주기 보고를 멈춥니다. 서버는 마지막 상태 요청을 받은 자체 시각도 저장하여 라우터 프로세스나 PC 자체가 꺼져 보고가 오지 않는 경우를 별도로 판정해야 합니다.

### 측정 데이터 처리

1. 수집기에 설정한 URL에서 JSON POST 요청을 받습니다. 기본 시험 주소는 `http://127.0.0.1:21000/ingest`입니다.
2. 최상위 `schema`를 확인해 데이터 형식에 맞는 처리 경로로 보냅니다. 온습도 외에도 일반 키-값 관측이나 두 형식을 함께 담은 요청이 들어올 수 있습니다.
3. 요청에 담긴 **모든** 묶음을 검사하고, 개별 ID를 기준으로 중복을 제거해 저장합니다.
4. 같은 거리 구간을 다시 받으면 **더 큰 `revision`의 내용으로 교체**합니다. 이전 누적 값에 새 누적 값을 더하지 않습니다.
5. 저장 트랜잭션이나 장애 후에도 복구할 수 있는 수신 큐의 커밋이 끝나면 **HTTP 204**를 반환합니다. 이미 저장한 자료를 다시 받은 경우에도 204를 반환합니다.

수집기는 204를 받으면 해당 요청에 담았던 자료를 RAM에서 지웁니다. 따라서 운영 서버는 요청을 읽기만 한 상태에서 204를 반환하면 안 됩니다. 아래에서 소개하는 시험 서버는 RAM의 데이터를 화면에 출력하는 도구이며, 운영용 저장 기능은 제공하지 않습니다.

```mermaid
sequenceDiagram
    participant P as PX4 / Pixhawk
    participant R as 수집 중계기
    participant S as 서버 수신 API
    participant D as 저장소 또는 내구성 큐
    P->>R: MAVLink 원본, 측정시각, 항법
    Note over R: 원본 수집과 거리 구간 갱신을 계속 수행
    opt UDP 백업 활성
        R->>S: 새 표본마다 같은 ID의 JSON 조각 송신
        S->>D: 재조립, 검사, 개별 ID 중복 제거 후 저장
    end
    Note over R: HTTP용 미확인 사본을 메모리 FIFO에 보존
    R->>S: 설정 주기마다 JSON POST, 대기 레코드를 묶어 송신
    S->>S: 검사, 개별 ID 중복 제거, revision 비교
    S->>D: 요청 전체 저장 트랜잭션
    D-->>S: 커밋 완료
    S-->>R: HTTP 204
    Note over R: 확인된 전송 사본 해제
```

## 2. 통신 방식

측정 데이터의 기본 저장 경로는 TCP 기반 HTTP/HTTPS입니다. 선택적으로 UDP 백업도 동시에 보낼 수 있으며, 아래 표와 응답 코드는 **HTTP 경로**의 계약입니다. UDP 수신 절차는 이 절의 뒷부분에 따로 설명합니다.

| 항목 | 계약 |
|---|---|
| 방향 | 수집기 → 지정 서버. 서버가 MAVLink를 직접 해독할 필요는 없음 |
| 전송 | TCP 기반의 HTTP 또는 HTTPS POST. 별도의 사설 길이 헤더는 사용하지 않음 |
| URL | `url` 설정의 전체 URL. `/ingest`·21000은 시험 기본값이며 운영 주소·포트는 변경 가능 |
| 본문 | UTF-8 JSON 문서 하나. gzip·CBOR 등의 압축·바이너리 전송은 지원하지 않음 |
| 헤더 | `Content-Type: application/json`, HTTP 스택이 구성한 본문 길이 |
| 인증 | 직접 입력한 `token`이 있으면 우선 사용. 비어 있으면 `token_env`에 적힌 **이름의 환경변수**에서 읽음. 값이 있으면 `Authorization: Bearer <값>`, 없으면 헤더 생략 |
| 토큰 제약·저장 | 최대 1,024바이트이며 CR/LF는 금지. Windows 설정 저장은 현재 사용자 DPAPI 암호문, Linux는 POSIX 권한을 지원하는 파일 시스템에 `0600`으로 원문 저장. 토큰 원문은 로그·Git·예제에서 제외 |
| HTTPS | OS의 WinHTTP 또는 libcurl이 제공하는 기본 서버 인증서 검증 사용. 별도 mTLS·인증서 고정·서명 헤더는 없음 |
| 응답 본문 | 수신 확인용 JSON이나 순번 필드는 불필요. 송신기는 응답 본문을 처리하지 않음 |
| 리다이렉트 | 자동 추적하지 않음. 실제 최종 URL을 설정해야 함 |
| 측정 요청 주기 | 기본 `send_interval_ms=1000`, 설정 범위 1,000..60,000ms. 보낼 측정 데이터가 없으면 측정 요청하지 않음. 선택 상태 보고는 별도 주기 |
| 요청 간격 | 기본 `http_min_interval_ms=500`: 직전 요청 완료 후 다음 요청 시작까지 최소500ms. 측정·상태·재시도·종료와 설정 재시작의 대기열 전송이 하나의 간격 제한을 공유. 응답 시간까지 더해지므로 기본 요청률은 초당 최대2회 수준 |
| 요청 분배 | 전송 주기에 고정한 대기 항목을 `max_windows` 단위로 묶어 위 간격에 따라 나눠 전송. 측정과 상태가 모두 준비되면 차례로 기회를 주며, 새 측정 항목은 다음 라운드에서 처리 |
| 재시도 | 미확인 측정 자료는 RAM에 보존. 401·403·429가 이어지면 공통 HTTP 대기를1,2,4,8,16,30초로 증가, 204 성공만 추가 대기 초기화. 다른 실패는 기존 대기를 유지하며 측정 실패에는 `send_interval_ms`도 적용. `Retry-After` 해석은 지원하지 않음 |
| 종료 시 | 입력이 끝난 상태에서 연속 실패 횟수가 3 이상이면 실패로 종료할 수 있음. 프로세스 종료나 전원 단절 시 RAM의 자료는 사라짐 |

Windows에서는 WinHTTP의 단계별 시간 제한을 사용합니다. Linux에서는 연결 시간 제한과 `connect+2×io`로 계산한 전체 HTTP 시간 제한을 사용하며, `connect`와 `io`의 기본값은 각각 1,000ms입니다. 직접 입력한 토큰은 TUI에서 `r`로 저장·적용합니다. URL이 같으면 보류 데이터도 새 인증 설정으로 먼저 재시도하므로, `401` 때문에 쌓인 데이터를 버릴 필요가 없습니다. URL도 바꾸면 보류 데이터에는 기존 목적지·인증 설정을 유지합니다. 서버는 User-Agent를 인증 수단이나 프로토콜을 구분하는 기준으로 사용하지 않아야 합니다.

대기 자료가 많아도 요청 간격 제한을 우회하지 않습니다. 상태 보고도 같은 제한을 사용하며, 설정 재시작 전후에도 직전 요청의 대기를 유지합니다. HTTP 대기 중 MAVLink 수신·분석과 UDP 백업은 계속 동작합니다. 이 제한은 **라우터 프로세스 하나**에 적용하므로 같은 서버에 여러 라우터가 보내면 서버가 보는 합계 요청률은 더 높을 수 있습니다.

`403`은 이 서버의 권한 정책이나 요청 제한 정책에 따른 거부일 수 있습니다. 응답 코드만으로 잘못된 토큰인지 초당 요청 수 제한인지 판별하지 않으며 서버 로그로 확인해야 합니다. `http_min_interval_ms`는 요청 수를 분산하는 설정이며 서버의 본문 크기 제한을 바꾸지는 않습니다. 큰 본문은 `max_windows`와 서버의 수신 상한을 함께 확인하세요.

토큰을 직접 입력하려면 `e` → `47` → Enter → Ctrl+U → 토큰 붙여넣기 → Enter → `r`을 사용하세요. Ctrl+U로 기존 입력을 먼저 지우면 이전 토큰 뒤에 새 값이 붙지 않습니다. `token` 입력은 가려지며 Windows 설정 파일에는 `token=dpapi:...` 형태로 저장합니다. 이 암호문은 현재 Windows 사용자에 묶이므로 다른 PC나 사용자로 옮기면 토큰을 다시 입력해야 합니다. Linux 저장 파일은 POSIX 권한을 지원하는 파일 시스템에서 소유자 읽기·쓰기 권한(`0600`)을 사용합니다. WSL에서는 Linux 홈 디렉터리처럼 이 권한을 지원하는 위치를 사용하세요. 이 인증값은 HTTP에만 사용하며 UDP 백업에는 포함하지 않습니다.

Linux 저장 시 파일 소유자·종류·`0600` 권한 검사가 실패하면 토큰을 기록하기 전에 저장을 거부하고 기존 설정 파일을 유지합니다. 환경변수 방식도 지원합니다. `token`을 비우고 `token_env=TB_TOKEN`을 유지한 뒤 라우터를 실행할 환경의 `TB_TOKEN`에 실제 인증값을 설정하세요. **`token_env`는 환경변수 이름을 지정하는 칸**으로, 영문자 또는 `_`로 시작하는 영문자·숫자·`_`만 최대64자까지 허용합니다. 인증이 없는 현재 로컬 시험 서버에는 토큰 설정이 필요 없습니다.

### 응답 코드

| 응답 | 송신기 동작 | 서버 사용 원칙 |
|---|---|---|
| **204 No Content** | 해당 요청에 들어 있던 모든 큐 항목 해제 | 전체 요청 커밋 또는 이미 커밋된 동일 자료 확인 후 반환 |
| 200·201·202 | 해제하지 않고 재시도 | 성공 응답으로 대체 사용하지 않음. 202 비동기 접수만으로 수집기 RAM을 해제할 수 없음 |
| 3xx | 보존·재시도, 자동 이동 없음 | 수집기에 최종 주소 설정 필요 |
| 401·403·429 | 보존·재시도, 공통 대기1초→최대30초 | 인증·권한·요청 제한 등 실제 거부 원인을 서버 로그로 확인. 403만으로 원인을 확정하지 않음 |
| 400·409·413·422 | 보존·재시도 | 원인을 서버 로그에 남기고 설정이나 자료의 불일치를 해결해야 함. 영구 오류로 분류해 해당 자료를 건너뛰지 않음 |
| 5xx·연결 실패·timeout | 보존·전송 주기와 기존 추가 대기를 지켜 재시도 | 장애 복구 후 동일 자료를 다시 받을 수 있음 |

저장이 끝났을 때의 응답 예시는 다음과 같습니다.

```http
HTTP/1.1 204 No Content
Content-Length: 0

```

TCP ACK는 네트워크 전송을 확인할 뿐, 서버가 자료를 저장했다는 뜻은 아닙니다. 서버가 저장을 마친 뒤 HTTP 응답이 유실되면 수집기는 같은 자료를 다시 보냅니다. 따라서 서버에는 중복을 안전하게 처리하는 기능이 필요합니다.

### TCP 전송 대기 메모리

HTTP로 아직 확인받지 못한 레코드는 RAM의 FIFO에 보존합니다. `tcp_buffer_max_mb=1024`가 기본값이며, 여기서 1MB 설정 단위는 1MiB(1,048,576바이트)입니다. 따라서 기본 한도는 **1GiB = 1,073,741,824바이트**입니다. TUI 46번 또는 설정 파일에서 변경한 뒤 `r`로 적용할 수 있습니다. 설정 범위는 1..65,536MiB이며 32비트 빌드에서는 주소 범위에 맞춰 최대 4,095MiB로 제한합니다.

이 한도에는 대기 레코드의 인코딩된 JSON·문자열 종료 바이트와 큐 노드 관리 정보가 포함됩니다. 프로그램 시작 시 1GiB를 미리 할당하지 않고, 새 자료가 쌓인 만큼 할당한 뒤 HTTP 204로 확인된 자료부터 해제합니다. 메모리 할당기의 내부 관리 공간, 현재 수집 중인 배치, UDP 작업 버퍼, HTTP 요청 작성 버퍼, 스택·OS·TLS 메모리는 별도이므로 **프로세스 전체 메모리를 1GiB 이하로 제한한다는 뜻은 아닙니다**.

화면의 `TCP memory`는 현재 사용량·상한·최대 사용량을 MiB로 표시합니다. 상세 화면의 `tcp_buffer`에는 `bytes`, `limit`, `high_water`, `allocation_failures`가 나오고, 거부된 레코드·표본 수는 `queue_windows`·`queue_samples` 손실 계수로 확인합니다.

서버 무응답·오류에는 기존 FIFO 항목과 ID를 보존합니다. 한도에 도달하거나 메모리 할당에 실패하면 새 HTTP용 항목을 거부하고 손실 수를 표시하며, 이미 대기 중인 항목을 덮어쓰지 않습니다. UDP 송신 시도는 계속됩니다. 디스크 저장은 하지 않으므로 프로그램 종료·전원 차단 뒤의 복구는 지원하지 않습니다.

### UDP 백업의 주소와 수신 형식

UDP 백업은 HTTP 정상 동작 중에도 병행합니다. HTTP 장애를 감지한 뒤에만 켜지는 경로가 아니며, HTTP 응답 대기나 재시도와 독립적으로 동작합니다. QGC에 원본 MAVLink를 전달하는 UDP 14550 경로와도 별개입니다.

```ini
url=http://127.0.0.1:21000/ingest
udp_backup_enabled=1
udp_backup_host=auto
udp_backup_port=0
tcp_buffer_max_mb=1024
```

위 설정에서는 HTTP를 `127.0.0.1:21000/ingest`, UDP를 `127.0.0.1:21001`로 보냅니다. `auto`는 HTTP URL의 호스트, 포트 `0`은 **HTTP TCP 포트+1**입니다. HTTP/HTTPS URL에서 포트를 생략하면 각각 UDP 81/444를 사용합니다. `udp_backup_host`와 `udp_backup_port`를 명시하면 따로 지정할 수 있습니다. TCP 포트 65535에는 자동으로 1을 더할 수 없으므로 UDP 포트를 명시해야 합니다. `url=-`에서는 UDP 호스트와 포트를 모두 명시해야 합니다. 기본 `udp_backup_enabled=0`은 비활성화입니다.

UDP 데이터그램은 JSON 문자열만 담는 형식이 아닙니다. **40바이트 `MTRU` v1 바이너리 헤더 + 최대 1,160바이트 JSON 조각**으로 이루어지며, 한 데이터그램은 최대 1,200바이트입니다. 헤더의 정수는 big endian입니다. 전체 JSON 크기, 조각 번호·개수·바이트 위치, 실행 nonce, 전송 시도 순번이 들어갑니다. 정확한 바이트 배치는 [UDP 백업 규약](UDP_BACKUP.md#데이터그램-헤더-v1)을 따릅니다. OpenAPI는 HTTP 엔드포인트를 설명하며 이 UDP 헤더를 정의하지 않습니다.

서버의 UDP 수신 절차는 다음과 같습니다.

1. 헤더의 magic·버전·길이·조각 범위를 검사합니다.
2. `(발신 IP, 발신 UDP 포트, 실행 nonce, 전송 시도 순번)`별로 조각을 모읍니다. 조각 순서가 바뀌거나 같은 조각이 다시 와도 처리해야 합니다.
3. 모든 조각이 모이면 바이트 단위로 재조립한 뒤 UTF-8 JSON을 해독합니다. 문자가 조각 경계에서 나뉠 수 있으므로 각 조각을 따로 문자열로 변환하지 않습니다.
4. 재조립한 JSON을 아래의 동일한 schema 검사·개별 ID 중복 제거·공간 revision 처리 경로로 넘깁니다. 부분 JSON을 저장하지 않습니다.

참조 수신기는 JSON 하나를 최대 2MiB, 동시 재조립을 최대 4개, 첫 조각 이후 대기를 2초로 제한합니다. 잘못된 조각이나 만료된 미완성 본문은 버립니다. UDP 헤더의 nonce·전송 순번은 **전송 조각 식별자**이며 관측의 중복 제거 키를 대신하지 않습니다.

UDP에는 HTTP URL의 경로·query·헤더·Bearer 토큰이 포함되지 않습니다. 자체 인증·서명·암호화 기능도 없으며 HTTP 주소를 HTTPS로 바꿔도 UDP는 암호화되지 않습니다. 운영 서버의 UDP 수신자 식별 범위는 서버가 별도로 정해야 합니다.

### 새 표본의 즉시 송신과 중복 처리

UDP 백업을 켜면 수집기가 해독·검증하여 수락한 일반 관측과 온습도 **표본마다 레코드를 확정하여 비차단 UDP 소켓으로 바로 송신을 시도합니다**. 별도의 UDP 대기 큐를 쌓거나 `send_interval_ms`·HTTP 응답을 기다리지 않습니다. 같은 ID·세션·원점·시계·관측 내용을 가진 JSON 사본은 TCP 대기 메모리에도 보관하고, HTTP 작업자가 설정 주기에 맞춰 여러 레코드를 묶어 보냅니다. UDP를 끄면 기존 측정 시간 구간·전송 주기에 따른 배치 수집을 사용합니다. 여기서 즉시는 관측 처리 이후 송신 대기를 생략한다는 뜻이며, 센서 자체 측정 주기나 운영체제의 실행 지연을 없앤다는 뜻은 아닙니다.

표본마다 확정하는 규칙은 실시간 MAVLink의 일반 관측 `schema:3`과 온습도에 적용합니다. 개발용 CSV `schema:2`는 `window_us`가 중복 제거 키이므로 UDP를 켜도 같은 측정 시간 구간의 배치를 유지하고, 배치가 확정될 때 UDP로 보냅니다. 같은 시간 구간의 표본을 서로 다른 CSV 배치로 나누어 서버가 정상 자료를 중복으로 버리게 하지 않습니다.

HTTP가 여러 레코드를 `records[]`·`batches[]`나 혼합 bundle로 묶더라도 **개별 레코드 ID와 측정 내용은 UDP와 같습니다**. 아래 9절의 개별 ID를 양쪽 수신 경로가 공유하는 원장에서 중복 제거하세요. UDP로 먼저 저장한 항목이 HTTP로 다시 들어와도 같은 관측을 두 번 추가하지 않고, 해당 HTTP 요청 전체가 저장된 것을 확인하면 204를 반환해야 합니다. 동일 ID인데 관측 내용이 다르면 정상 중복으로 덮어쓰지 말고 충돌로 기록합니다. 공간 구간은 전송 경로와 무관하게 더 큰 `revision`으로 교체합니다.

UDP 소켓을 사용할 수 없거나 커널 송신 버퍼가 차는 등 송신에 실패하면 누락·오류를 집계하고 그 항목을 재시도하지 않습니다. 따라서 모든 수락 표본에 대해 송신을 시도하는 정책이지, UDP의 무손실 도착을 보장하는 정책은 아닙니다. 원시 MAVLink 프레임 전체를 보내는 기능도 아닙니다.

### 두 전송 경로의 수신 확인과 상태 보고

UDP 수신 서버는 응답이나 ACK를 보내지 않습니다. 수집기의 `sent`는 모든 조각을 로컬 소켓에 넘겼다는 뜻이며 서버의 수신·저장 성공을 뜻하지 않습니다. UDP 실패를 재시도하지 않고, UDP 성공으로 HTTP 대기 데이터를 해제하지도 않습니다. HTTP에서는 전체 요청의 저장 또는 이미 저장된 동일 자료 확인을 마친 뒤 **204**를 반환해야 합니다.

`status_interval_ms=10000`이면 새 센서 표본이 없어도 연결 상태를 약 10초마다 보냅니다. HTTP와 UDP는 같은 상태 JSON 형식과 순번 발급기를 쓰지만 **각각의 송신 시각에 만든 별도 스냅샷**입니다. 따라서 두 경로에서 같은 개수나 연속 순번을 받을 것이라고 가정하지 마세요. 상태의 `sample_count=0`은 유지되며, 마지막 센서 값과 STALE 위치를 새 관측으로 더하지 않습니다. 늦게 도착한 과거 상태가 최신 상태를 덮어쓰지 않도록 세션·세대·보고 시각과 순번을 함께 비교하세요. `run_elapsed_ms`는 실행 시간이고 `rx_age_ms`는 마지막 기기 데이터 수신 이후 시간입니다.

## 3. 요청을 읽는 순서와 버전 분기

JSON 객체의 필드 순서는 처리 결과에 영향을 주지 않습니다. 다만 일반 관측의 `raw` 배열 안에서는 `field_order`가 정한 열 순서를 따라야 합니다. 호환성을 위해 새 객체 필드는 허용하되, 계산에는 의미를 아는 필드만 사용하세요. 지원하지 않는 `schema`나 SI 단위 사전 버전을 받았을 때는 자료를 버리고 204를 반환해서는 안 됩니다.

| 최상위 `schema` | 용도 | 서버 진입점 |
|---|---|---|
| `"mavlink_weather_v1"` | 온습도 원본·시간 요약·선택 거리 구간 | `records[]` |
| 숫자 `3` | 실제 MAVLink에서 수집한 일반 키-값 관측 | `batches[]` |
| `"mavlink_router_bundle_v1"` | 온습도와 일반 관측이 함께 대기한 운용 요청 | `records[].kind`와 `records[].payload` 안에 담긴 원래 요청 객체 |
| `"mavlink_router_test_v1"` | 하트비트·좌표 시험 모드 | `records[]` |
| `"mavlink_router_status_v1"` | 새 표본 유무와 별개인 연결·센서·위치 상태. 관측 수에 더하지 않음 | `records[]` |
| 숫자 `2` | CSV 개발용 재생 | `batches[]`; 운영 실시간 자료와 구분 |

혼합 요청의 바깥 객체에는 `stream`과 `session`이 없습니다. 각각의 `payload`에 담긴 값을 사용하세요.

```json
{
  "schema": "mavlink_router_bundle_v1",
  "records": [
    {"kind": "telemetry", "payload": {"schema": 3, "...": "아래 일반 envelope 전체"}},
    {"kind": "weather", "payload": {"schema": "mavlink_weather_v1", "...": "아래 온습도 envelope 전체"}}
  ]
}
```

위 코드는 형식을 설명하기 위해 줄인 예시입니다. 실제로 POST할 때는 [api/](api/README.md)의 합성 `.json` 예제를 사용하세요.

### 정수와 미상 값

`U64String`은 `0..18446744073709551615` 범위의 정수를 십진 문자열로 표현한 것입니다. `U32`는 `0..4294967295` 범위의 정수입니다. `U64String`을 비교할 때는 문자열 순서가 아니라 숫자 크기를 사용해야 합니다. 예를 들어 문자열 정렬에서는 `"10" < "2"`가 되므로 `revision`을 잘못 판단할 수 있습니다.

현재 구현은 다음과 같이 64비트 정수를 문자열과 JSON 숫자로 나누어 표현합니다.

- 부팅 기준·UTC 시각, 일반 `batch_id`, 누적 `loss`, 공간 ID·`revision`·누적 카운터: JSON **문자열**.
- 온습도 `records[].seq`, 하트비트의 `seq`·`source_generation`·단조 시각·`boot_ms`: JSON **number**이며 내부 저장형은 64비트일 수 있습니다.
- 일반·온습도 개별 표본의 `seq`, 품질 비트·개수·대부분의 센서 ID: U32 범위의 JSON number.

JavaScript/TypeScript 서버에서 일반 `JSON.parse`만 사용하면 일부 64비트 숫자의 정밀도가 손실될 수 있습니다. 손실 없는 정수 파서를 사용하거나, JSON 원문의 숫자 토큰을 BigInt 또는 십진 문자열로 보존하세요. 데이터베이스의 부호 있는 64비트 `BIGINT`도 U64 전체 범위를 담지 못하므로, 20자리 숫자형이나 문자열 등 범위를 보존할 수 있는 저장형을 명시해야 합니다.

JSON의 `null`은 값을 모르거나 계산할 수 없다는 뜻입니다. 값·좌표·경과 시간이 0인 경우와 구별해야 합니다. 송신기는 `NaN`이나 `Infinity`를 문자열 또는 리터럴로 보내지 않습니다. 온습도 구조와 `raw_clock_anchor`에서는 알 수 없는 시각 오차를 `null`로 보냅니다. 반면 일반 `schema:2`와 `schema:3`의 원본 상대 시각 오차에는 별도의 허용 범위가 있습니다. 이 필드가 내부 U32의 미상 표시값을 그대로 보내는 것으로 해석해서는 안 됩니다.

## 4. 온습도 요청 객체

`records[]`의 각 원소는 다음 필드를 가집니다.

| 필드 | 형식 | 의미 |
|---|---|---|
| `seq` | U64 JSON number | 내용이 확정된 전송 레코드의 ID. 개별 센서 표본 순번과는 다름 |
| `generation` | U32 | 입력/부팅 세대 |
| `session` | string | 설정 이름·실행 난수·세대를 조합한 값. 프로그램을 다시 실행했을 때 같은 센서의 이전 자료와 구분 |
| `source.sysid`, `source.compid` | 정수 | MAVLink system/component 식별자 |
| `frame` | `"ENU"` | 위치·속도 성분 순서 East, North, Up |
| `origin_wgs84` | `[lat_deg,lon_deg,alt_msl_m]` 또는 null | 세션별 지역 원점. 위경도는 WGS84, 실시간 고도는 MSL |
| `time_basis` | `"pixhawk_boot_us"` | 표본 시각은 해당 Pixhawk 부팅 시계 |
| `host_time_basis` | `"collector_monotonic_ms"` | 전송 묶음의 호스트 시계. UTC가 아님 |
| `clock_model` | 모델 객체 또는 null | boot↔UTC 정렬 진단. 아래 시각 절 참고 |
| `weather` | 객체 | 원본과 이번 호스트 전송 구간의 시간 통계 |
| `spatial` | 선택 객체 | `spatial_enabled=1`일 때 거리 누적 통계. 내부 schema는 `mavlink_spatial_v1` |

바깥 요청 객체에는 `schema`, `stream`, `records`가 있습니다. HTTP 온습도 요청 하나에는 최대 `max_windows`개의 레코드를 묶을 수 있으므로 서버는 `records`의 모든 원소를 처리해야 합니다. UDP는 확정된 레코드마다 한 개씩 보냅니다. 이 온습도 요청 객체가 혼합 요청의 `payload` 안에 담겨 올 수도 있습니다.

### `weather`

| 필드 | 의미 |
|---|---|
| `si_registry=1` | 아래 두 물리량의 단위 사전 |
| `host_window_ms` | 수집기 단조 시각의 묶음 시작을 나타내는 U64String. 측정시각·UTC·거리 구간 ID가 아님 |
| `n`, `raw_n`, `raw_dropped` | 수락 관측 수·보존 원본 수·원본 한도 초과 수. `n=raw_n+raw_dropped` |
| `measurements.temperature[]` | 센서/프로토콜별 온도 그룹, 단위 K |
| `measurements.humidity[]` | 센서/프로토콜별 상대습도 그룹, 단위 0..1 비율 |

실제 센서가 아직 특정되지 않았더라도 `sensor_id=0`은 유효한 논리 ID입니다. `message_id=8009`는 공식 SENS_ATMOS, `12920`은 HYGROMETER_SENSOR를 뜻합니다. 관측 그룹은 `(metric,message_id,sensor_id,sensor_instance)`로 구분하므로 서로 다른 메시지나 센서 인스턴스를 하나의 평균으로 섞지 마세요. 공식 SENS_ATMOS에는 물리 센서의 고유 ID가 없기 때문에 `(0,0)`은 수집기에서 다루는 단일 논리 입력을 나타냅니다.

각 센서 그룹에는 다음 필드가 있습니다.

| 필드 | 내용 |
|---|---|
| `message_id`, `sensor_id`, `sensor_instance` | 위 그룹 식별자 |
| `representative` | 실제 표본 하나. 아래 표본 객체와 동일 |
| `statistics` | `n,mean,variance,min,max,time_known_n,t_start_us,t_end_us` |
| `raw_n`, `raw_dropped` | 이 그룹의 원본 보존/누락 수 |
| `samples[]` | 보존한 실제 원본 표본 |

`variance`는 `n≥2`일 때의 표본분산이며, `n=1`이면 `null`입니다. `t_start_us`와 `t_end_us`는 측정시각을 아는 표본들의 최소·최대 시각이고, `time_known_n`은 그 표본 수입니다. 모든 표본의 시각이 미상이면 시각 범위도 `null`입니다.

통계 평균은 여러 표본을 집계한 값이므로 대표 표본의 좌표에서 직접 측정한 값으로 취급하면 안 됩니다. `raw_dropped>0`이면 원본 일부가 빠졌더라도 통계에는 포함될 수 있습니다. 이때 보존 원본만으로 다시 계산한 평균이 전체 통계 평균과 다르다고 해서 오류로 판단하지 마세요.

### 원본/대표 표본 객체

| 필드 | 형식·단위·해석 |
|---|---|
| `t_us` | U64String 또는 null, 센서 관측의 Pixhawk boot µs |
| `seq` | U32, 수집기가 부여한 개별 표본 순번 |
| `value` | 온도 K 또는 상대습도 비율. %값으로 중복 변환하지 않음 |
| `position_m` | ENU `[east,north,up]` m 또는 null |
| `velocity_mps` | ENU `[vE,vN,vU]` m/s 또는 null. 배열 존재만으로 속도 유효성이 확정되지 않음 |
| `velocity_valid` | boolean, 해당 관측시각에 공간 경로 계산에 쓸 속도 유효성 근거가 있음 |
| `navigation_segment` | U32, 항법의 연속 구간. 0도 유효 ID이며 다른 구간을 이어 적분하지 않음 |
| `position_source` | `GPS`, `ESTGPS`, `UNKNOWN` |
| `estimation_age_us` | 마지막 유효 GPS 기준점 이후 경과 U32 µs 또는 null. GPS 표본은 0 |
| `measurement_variance` | 해당 단일 표본의 측정 분산, K² 또는비율². 미상이면 null |
| `time_uncertainty_us` | 관측시각의 상대 오차 추정 U32 µs 또는 null |
| `navigation_uncertainty_m` | 위치 항법 오차 지표 m 또는 null |
| `position_uncertainty_m` | 알려진 항법 오차+속력×시각오차 기반 지표 m 또는 null |
| `quality` | U32 비트마스크. 정수 크기를 정확도 순위로 정렬하지 않음 |

온습도 품질 비트는 다음과 같습니다. 여러 비트가 동시에 설정될 수 있습니다.

| 비트 | 의미 |
|---|---|
| `0x04000000` | 온습도 측정 위치 미상 |
| `0x08000000` | 온습도 측정시각 미상 |
| `0x10000000` | ESTGPS 추정 위치 |
| `0x20000000` | 해독기의 미인증 원천 시계 플래그. 시각 존재/UTC 정확도 보증과 별개 |
| `0x40000000` | 일반 해독기의 보수적 위치 품질/오차 미상 플래그. 그 자체를 `position_m=null`과 동치로 취급하지 않음 |

아직 정의를 모르는 품질 비트도 보존하세요. 위치와 시각을 알 수 있는지는 해당 필드와 온습도 전용 미상 비트를 함께 확인해야 합니다. HYGROMETER_SENSOR에는 관측시각이 없으므로 `t_us`와 좌표가 `null`일 수 있습니다. 이를 서버 접수 시각이나 직전 위치로 채우면 안 됩니다.

`GPS`는 GPS의 지지를 받는 비행제어기(FC)의 융합 위치라는 분류이며, 원시 GPS 표본을 뜻하지 않습니다. `ESTGPS`의 오차가 `null`인 경우에도 0으로 바꾸지 마세요.

ENU 좌표를 위경도로 변환하거나 보관할 때는 원점도 함께 보존해야 합니다. MSL 고도를 타원체 고도로 해석하거나 ENU 벡터를 위도·경도 배열로 읽지 마세요. ENU 숫자가 같아도 원점이나 세션이 다르면 같은 장소를 뜻하지 않습니다.

## 5. 거리 구간 `spatial`

기본 전송 주기는 1초이고, 기본 거리 구간의 목표 폭은 수평 경로를 따라 10m입니다. 전송할 때도 아직 진행 중인 구간은 초기화하지 않습니다. 빠르게 이동하면 한 요청에 여러 구간이 담기고, 느리게 이동하면 같은 구간에 더 많은 표본이 쌓입니다. 단위와 원본 표본의 구조는 앞 절과 같습니다.

| 필드 | 내용 |
|---|---|
| `schema` | `"mavlink_spatial_v1"` |
| `si_registry` | 1 |
| `target_step_m` | 목표 수평 구간 폭 m, 기본 10 |
| `adaptive` | 실제 관측 간격에 따라 다음 구간 폭을 조정하는지 |
| `gap_us` | 경로를 잇는 관측 간격 정책, U64String µs |
| `path` | 최신 `known,path_id,bin_index,s_m,effective_width_m`. ID는 U64String |
| `counters` | 아래 누적 진단. 대부분 U64String, `flags`는 U32 |
| `bins[]` | 이번 전송의 완료/중단 구간과 새 표본으로 갱신된 진행 구간, 최대 144레코드 |

`counters`에는 `samples_seen,spatial_samples,unknown_samples,rejected_samples,late_samples,paths_started,completed_records,dropped_records,undersampled_steps,inconsistent_steps,flags`가 있습니다. 이 값은 최신 누적 상태를 보여 주므로 전송마다 다시 더하면 안 됩니다. `path.known=false`일 때 `s_m=0`이나 `effective_width_m=0`이라고 해서 실제 해상도가 0m라고 해석해서도 안 됩니다.

`bins[]`의 각 원소는 다음 필드를 가집니다.

| 필드 | 내용 |
|---|---|
| `continuity_key,path_id,bin_index,revision` | U64String 식별자/누적 개정 번호 |
| `metric` | 현재 `temperature` 또는 `humidity` |
| `message_id,sensor_id,sensor_instance,source_kind` | 그룹·출처 식별. source_kind는 GPS/ESTGPS |
| `state` | `partial`: 진행 중, `complete`: 거리 경계 통과, `interrupted`: 연속성 단절 |
| `flags`, `quality` | 구간 진단 플래그와 관측 품질 비트마스크 |
| `width_m` | 이 구간의 명목 폭. 이미 열린 구간에서는 바뀌지 않음 |
| `interval_start_m,interval_end_m` | 경로상 명목 구간 범위 |
| `observed_start_m,observed_end_m` | 실제 표본이 차지하는 경로 범위. 명목 경계에 표본이 있다는 뜻이 아님 |
| `entry_step_m` | 해당 구간 진입 시 관측된 표본 간 이동거리 |
| `t_start_us,t_end_us` | 실제 포함 표본의 boot 시각 범위, U64String |
| `statistics` | `n,mean,m2,variance,min,max`; `variance=m2/(n−1)`, n=1이면 null |
| `representative` | 실제 포함 표본 하나의 전체 객체. 평균의 가상 관측이 아님 |

기본 경로 길이는 유효성이 확인된 수평속력을 사다리꼴 적분해 구합니다. 위경도의 흔들림을 이동거리로 그대로 더하지 않습니다. 적응형 구간 폭은 `max(목표 폭, 관측된 표본 간 거리)`를 기준으로 다음 구간부터 조정합니다. 폭을 줄이기 위한 확인 절차나 큐 포화 보호 때문에 이 기준보다 넓어질 수도 있습니다.

원본 표본이 없는 빈 구간이나 경계 위의 가상 센서 값은 생성하지 않습니다. `width_m`이 10m여도 실제 대표 표본 사이의 거리가 항상 10m인 것은 아니며, 센서 자체의 공간 해상도가 10m라는 의미도 아닙니다.

### 공간 상태 플래그

| 값 | 이름 | 의미 |
|---|---|---|
| 1 | UNDERSAMPLED | 표본 간격이 목표 폭을 넘은 구간 존재 |
| 2 | MOTION_UNKNOWN | 위치·속도 일치성 근거 부족 |
| 4 | NAV_UNKNOWN | 항법 근거 미상 |
| 8 | VELOCITY_UNKNOWN | 유효 속도 미상 |
| 16 | TIME_GAP | 관측 간격 한도 초과 |
| 32 | CONTEXT_CHANGE | 부팅·입력·원점·항법 구간 등 연속성 변경 |
| 64 | SOURCE_CHANGE | GPS/ESTGPS 등 출처 변경 |
| 128 | OUT_OF_ORDER | 경로에 반영하지 않은 역순 표본 |
| 256 | RATE_LIMITED | 공간 완료 큐 용량 압력 |
| 512 | COUNT_OVERFLOW | 카운터/ID 표현 상한 |
| 1024 | CONFIG_CHANGE | 모듈 설정 변경 |
| 2048 | PATH_ESTIMATED | 속도 적분 기반 경로. source_kind의 GPS/ESTGPS와 별개 |

### 공간 구간 저장 키와 갱신

서버가 인증으로 확인한 송신자 또는 테넌트를 `principal`이라고 할 때, 거리 구간은 다음 논리 키로 구분하는 것을 권장합니다.

```text
principal, stream, session, source.sysid, source.compid,
continuity_key, path_id, bin_index,
metric, message_id, sensor_id, sensor_instance, source_kind
```

`principal`은 요청 본문에서 가져오는 값이 아니라 인증이나 서버 설정으로 결정하는 값입니다. 여러 기체의 구간을 `bin_index` 하나만으로 구분해서는 안 됩니다.

같은 키의 구간을 다시 받으면 다음과 같이 처리합니다.

```text
incoming revision > stored revision : 통계·대표값·state를 통째로 교체
incoming revision = stored revision : 동일 의미면 중복 처리
incoming revision < stored revision : 최신값을 유지하고 오래된 갱신은 무시
동일 revision인데 관측 내용이 충돌 : 정상 중복으로 덮어쓰지 말고 불일치 기록
```

예를 들어 같은 구간에 대해 `partial n=10,rev=10` → `partial n=20,rev=20` → `complete n=20,rev=21`을 받았다면 최종 표본 수는 20입니다. 세 값을 더해 50으로 저장하면 안 됩니다. 더 큰 `revision`인지를 판단하고 교체하는 과정은 원자적인 조건부 갱신이나 트랜잭션으로 처리하세요. 그래야 요청이 동시에 들어오거나 도착 순서가 뒤집혀도 최신값이 유지됩니다.

온습도 시간 통계와 공간 통계는 같은 관측을 서로 다른 기준으로 집계한 결과입니다. 두 통계의 `n`을 더하면 같은 표본을 중복 계산하게 됩니다. 서로 다른 완료 구간을 합칠 때도 평균끼리 단순히 평균 내면 안 됩니다. 표본 수를 가중치로 사용하고 다음 M2 병합식을 적용하세요.

```text
n = nA + nB
delta = meanB - meanA
mean = meanA + delta*nB/n
M2 = M2A + M2B + delta²*nA*nB/n
```

이 식은 서로 겹치지 않는 자료를 합칠 때만 사용합니다. 같은 구간의 여러 `revision`은 합치는 대상이 아닙니다. 또한 느리게 이동해 표본 수가 늘더라도 센서 응답 특성과 자기상관의 영향을 고려해야 합니다. 표본 수의 증가를 독립 관측 수나 정확도의 향상으로 바로 환산하지 마세요.

## 6. 일반 키-값 관측 `schema:3`

운용 중에는 미세먼지처럼 새로운 관측 키가 추가될 수 있습니다. 따라서 서버는 온습도 필드만 들어온다고 가정해서는 안 됩니다. 현재 해독기와 단위 설정이 제공하는 키·단위를 그대로 보존하고, 각 키가 뜻하는 물리량은 별도 사전으로 관리하세요. 다만 수집기가 정의를 모르는 MAVLink 메시지까지 자동으로 키-값 관측으로 바꾸는 것은 아닙니다.

| 요청 객체의 필드 | 내용 |
|---|---|
| `schema=3`, `stream`, `session` | 실시간 일반 관측 분류 |
| `frame,origin_wgs84` | ENU 지역 원점. 실시간 고도는 MSL |
| `source` | `sysid,compid,epoch,altitude_datum:"MSL"` |
| `time_basis,time_epoch,utc_basis` | `pixhawk_boot_us`,`session_boot`,`unix_us_estimate` |
| `field_order` | 아래 17개 표본 배열 열 이름 |
| `loss` | `rejected,sequence_gaps,queue_windows,queue_samples,raw_samples`, 누적 U64String |
| `clock_model` | 모델 또는 null |
| `raw_clock_anchor` | 선택 원시 boot/UTC 정렬점. `boot_us,utc_us` 문자열·`uncert_us` 정수/null |
| `batches[]` | 개별 중복 처리 단위 |

각 배치에는 `batch_id`(U64String), 내용 확정 시점의 `clock_model`, 선택적인 `raw_clock_anchor`, `window_us`(U64String), `n,raw_n,raw_dropped,key_n,raw_included`, `stats[]`, `raw[]`가 있습니다. 자료가 늦게 도착하거나 전송을 마감하면서 같은 측정 시간대의 관측이 여러 배치로 나뉠 수 있습니다. 따라서 **`window_us`만으로 중복을 제거하면 안 됩니다**.

`stats[]`는 키와 단위별로 `key,unit,n,mean,variance,min,max,time_uncert_mean_us,time_uncert_max_us,position_error_n,position_error_mean_m,position_error_max_m`를 담습니다. 여기서 `n`은 통계 계산에 포함한 유한 관측 수이므로 배치 전체의 `n`과 다를 수 있습니다. `value=null`인 표본은 배치의 `n`과 `raw`에는 포함되지만 `stats.n`에서는 제외됩니다.

`n=0`이면 평균·분산·최솟값·최댓값·시각 오차 통계는 `null`입니다. `position_error_n=0`이면 위치 오차의 평균과 최댓값도 `null`입니다. M2는 직접 보내지 않으므로 필요한 경우 `n`과 표본분산으로 계산합니다.

`raw[]`의 각 원소는 다음 순서의 17열 배열입니다.

| 위치 | 이름 | 형식 |
|---|---|---|
| 0 | `t_us` | boot µs U64String |
| 1 | `seq` | U32 |
| 2 | `quality` | U32 |
| 3 | `key` | ASCII 키 |
| 4 | `unit` | 단위 문자열 |
| 5..7 | `east_m,north_m,up_m` | ENU m |
| 8..10 | `ve_mps,vn_mps,vu_mps` | ENU m/s |
| 11 | `value` | number 또는 null |
| 12 | `time_uncert_us` | 기본 빌드에서 0..20,000µs만 수락. 미상 UINT32_MAX를 보내는 필드가 아님 |
| 13 | `nav_age_us` | 기본 빌드에서 0..200,000µs만 수락 |
| 14 | `pos_error_m` | number 또는 null |
| 15 | `utc_us` | UNIX UTC µs U64String 또는 null |
| 16 | `utc_uncert_us` | U32 µs 또는 null |

키는 최대 31자, 단위는 최대 15자입니다. 다만 MAVLink의 NAMED_VALUE_* 메시지에서 전송되는 이름 자체는 최대 10바이트입니다. **일반 키-값 관측은 모든 값이 SI 단위로 통일되어 있지 않습니다.** 압력이 hPa, 온도가 degC로 들어올 수 있습니다. `unit_map`은 표시 단위를 지정할 뿐 원본 값을 SI로 변환하지 않습니다. 서버에서 단위를 바꾼다면 원본 값과 단위를 보존하고 변환한 값은 별도로 관리하세요.

`summary=1`이면 일반 관측의 `raw` 배열을 비우고 `raw_included=false`로 통계만 보낼 수 있습니다. 이때 `raw_n`은 수집기가 내부에 보존한 원본 개수이므로 실제 전송한 `raw[]` 길이와 다를 수 있습니다. 이 설정은 온습도 원본에는 적용되지 않습니다.

## 7. 하트비트 시험·CSV 재생

`mavlink_router_test_v1` 요청은 `stream`과 `records[]`로 구성됩니다. 각 레코드에는 `seq,source_generation,session,source,heartbeat,position,coordinate_age_ms`가 있고, `source`는 연결 주소를 나타내는 문자열입니다.

`heartbeat`에는 `sysid,compid,type,autopilot,base_mode,custom_mode,system_status,received_monotonic_ms`가 들어갑니다. `position`은 `lat_deg,lon_deg,alt_msl_m,boot_ms,received_monotonic_ms`를 가진 객체이거나 `null`입니다. 좌표가 `null`이면 `coordinate_age_ms`도 `null`입니다.

여기에 담긴 위치는 특정 하트비트가 생성된 순간의 동시 관측이 아니라, 수집기가 가장 최근에 수락한 항법 상태입니다. 이 시험 형식에는 GPS/ESTGPS를 구분하는 출처 필드가 없으므로 원시 GPS나 검증된 GPS 위치라는 의미를 추가하면 안 됩니다. 기기 종류는 하트비트의 열거형 값으로 식별하고, 컴포넌트 번호만으로 추측하지 마세요.

숫자 `schema:2`는 개발 중 CSV를 재생할 때 사용합니다. 17열 원본 배열과 기본 통계 구조는 일반 관측과 같지만, 실시간 형식의 `source`, `batch_id`, 실행 세대 규칙은 없습니다. 이 모드의 배치는 `window_us`로 구분합니다. 운영 수집과 다른 스트림을 사용하고, 좌표 원점과 고도의 의미는 CSV를 제공하는 쪽의 규칙을 확인하세요.

## 8. 시각과 UTC

- `t_us`는 센서 또는 FC의 부팅 시계를 기준으로 한 **관측시각**입니다. 서버 수신시각으로 바꾸지 마세요.
- `host_window_ms`와 `received_monotonic_ms`는 수집기나 연결 상태를 진단하는 단조 시각입니다. 다른 PC의 시계나 UTC와 직접 비교할 수 없습니다.
- `clock_model=null`이면 시각을 Unix epoch로 간주하거나 자릿수만 보고 UTC라고 추정하지 마세요.
- 재부팅이나 세대 전환 전후에는 부팅 기준 숫자가 같아도 서로 다른 시각입니다. 세션과 `source`를 함께 저장하세요.
- 서버가 수신한 UTC를 별도 열로 기록할 수는 있지만 관측 UTC를 그 값으로 덮어쓰면 안 됩니다.

`clock_model`에는 다음 필드가 있습니다.

| 필드 | 의미 |
|---|---|
| `anchors` | 사용한 정렬점 수, 최신 최대 16개 |
| `oldest_boot_us,last_boot_us,last_utc_us` | U64String, 정렬점 범위와 마지막 원시 UTC |
| `offset_us` | signed 십진 문자열, 마지막 boot에서 평가한 반올림 UTC−boot |
| `drift_ppm` | 중심화 OLS 기울기의 ppm |
| `residual_std_us,max_residual_us` | 잔차 진단 |
| `anchor_uncert_max_us` | 제공된 anchor 오차 중 최대 U32 µs |
| `rtt_mean_us,rtt_max_us` | 정렬 통신 왕복 지연 진단 |

온습도 표본에는 개별 UTC를 직접 담지 않습니다. 서버가 UTC를 추정한다면 사용한 모델과 가정, 오차도 함께 보존해야 합니다. 내부 모델은 ±500ppm 필터를 적용하고, 마지막 시각 기준점 이후에는 최대 10초까지만 외삽합니다.

기준점 수가 적을 때 `residual_std`가 0이더라도 시계 오차가 0이거나 정확도가 인증되었다는 뜻은 아닙니다. 마지막 원시 UTC에 부팅 시각의 차이만 더하거나 오프셋만 더한 결과를 확정 UTC로 저장하지 마세요.

## 9. 서버의 저장·중복 처리 절차

개별 자료의 수신 여부는 다음 키로 구분하는 것을 권장합니다.

| 종류 | 요청 내부에서 찾을 키 |
|---|---|
| 온습도 | `(principal,stream,session,source,kind="weather",record.seq)` |
| 일반 실시간 | `(principal,stream,session,source,kind="telemetry",batch.batch_id)` |
| 하트비트 시험 | `(principal,stream,session,kind="test",record.seq)` |
| CSV 재생 | `(principal,stream,session,kind="csv",batch.window_us)` |
| 혼합 | 바깥 bundle 자체의 ID는 없음. 내부 payload의 위 키들을 사용 |

`source`는 sysid·compid와 제공된 세대를 포함해 일관된 형태로 정규화하세요. 여러 기체나 사용자가 같은 `stream` 이름을 쓸 수 있으므로, 인증된 `principal` 또는 서버가 정한 수신자 범위를 키 앞에 두어야 합니다. `seq`와 일반 `batch_id`는 공유 카운터를 쓸 수 있습니다. 번호가 연속하지 않더라도 곧바로 데이터 손실이라고 판단해서는 안 됩니다.

서버 언어나 데이터베이스에 관계없이 다음 순서로 처리하면 됩니다.

```text
POST(request):
    principal = authenticate_or_resolve_configured_source(request)
    envelope = parse_json_without_losing_64_bit_integers(request.body)
    items = validate_and_flatten_all_supported_envelopes(envelope)
    BEGIN TRANSACTION
        for item in items:
            key = stable_item_key(principal, item)
            if key already committed:
                verify same immutable observations; do not append them again
                continue
            insert receipt key and original observation payload
            for bin in item.spatial.bins:
                conditionally replace if incoming revision > stored revision
                verify equal-revision conflicts; keep newer stored revisions
        COMMIT
    return HTTP 204 with empty body
```

수신 원장, 원본 표본, 전송 시간 구간의 요약, 거리 구간의 최신값, 시계 모델과 원점은 구분해 저장하세요. 데이터베이스 종류는 아직 정하지 않았습니다. 원본 표본을 별도 테이블로 나누어 저장할 때는 부모 수신 ID와 표본 그룹·`seq`를 고유 키에 포함해야 재전송으로 같은 표본이 추가되는 일을 막을 수 있습니다. 수신 원장의 키 삽입과 거리 구간의 `revision` 갱신은 같은 트랜잭션으로 커밋해 일부 처리만 빠지지 않도록 하세요.

**내용이 바뀌지 않는 단위는 개별 전송 묶음입니다. 전체 HTTP 본문이 매 재시도마다 같은 바이트로 유지되는 것은 아닙니다.** 재시도를 기다리는 동안 새 묶음이 추가되거나, 온습도와 일반 관측이 혼합 요청으로 다시 포장될 수 있습니다. 현재 송신기는 `loss`를 포함한 각 레코드의 JSON을 확정 시점에 보존합니다. 따라서 전체 본문의 해시만으로 중복을 판단하면 안 됩니다. 같은 개별 ID를 비교할 때는 관측·원점·내용 확정 시점의 시계 모델 등 자료 자체의 의미를 확인하세요. 객체 키 순서나 바깥 포장 차이를 관측 충돌로 판단하지 마세요.

한 요청에서 일부 항목만 커밋했다면 204를 반환하면 안 됩니다. 전체를 롤백하거나, 이미 커밋한 개별 ID를 원장에 남겨 요청 전체를 재시도해도 안전하게 이어받을 수 있어야 합니다. 송신기는 개별 항목의 수신 확인이나 부분 성공 응답을 지원하지 않습니다.

## 10. 크기·부하·손실 처리

현재 기본 빌드의 용량 제한은 다음과 같습니다.

| 상한 | 값 |
|---|---|
| 송신 본문 버퍼 | 1,110,016B, NUL 제외 본문 최대 1,110,015B |
| 미확인 TCP 큐 | 기본 `tcp_buffer_max_mb=1024`: JSON·노드 관리 정보 합계 1GiB. 동적 할당하며 고정 8개 제한은 없음 |
| UDP 송신 대기 큐 | 없음. 항목마다 비차단 소켓 송신 시도, 실패 항목은 카운트 후 재시도하지 않음 |
| 기본 요청 선택 | 최대 4개 묶음. 온습도와 일반 관측이 함께 대기하면 `max_windows=1`이어도 2개를 선택할 수 있음 |
| 일반 배치 | 원본 512개·키 16개 |
| 온습도 호스트 배치 | 원본 128개·그룹 16개 |
| 공간 상태 | 진행 16그룹·완료 128레코드·전송 사본 144레코드 |

운영 수신 API와 리버스 프록시의 본문 허용량은 수집기의 송신 상한보다 크게 설정하세요. 예를 들어 2MiB로 정하고, 실제 적용한 제한을 문서에 남기면 됩니다. 수집기의 큐가 가득 차면 새 배치를 버릴 수 있으며, 원본 보존 한도를 넘은 수량도 손실 카운터로 나타납니다. 수집기는 RAM만 사용하므로 장시간 서버 장애가 나도 모든 자료를 보존하거나 재시작 후 재전송한다고 보장할 수 없습니다.

JSON 구조 검사의 연산량은 O(본문 바이트+토큰 수)입니다. 개별 수신 키와 거리 구간 키에는 적절한 고유 인덱스를 두어 처리량이 받은 자료 수에 비례하도록 설계하세요. 요청마다 관측 테이블 전체를 다시 읽는 구조는 피해야 합니다. UDP를 켜면 표본마다 레코드가 생기고 HTTP 한 주기에도 여러 요청을 보낼 수 있으므로, 전송 주기만으로 서버의 요청 수를 초당 1회로 가정하면 안 됩니다.

작업 큐를 사용할 때는 자료가 메모리 큐에 들어간 상태와 장애 후 복구 가능한 상태로 커밋된 시점을 구별해야 합니다. 후속 비동기 처리까지 기다리지 않고 응답하려면, 복구 가능한 수신 원장이나 큐의 커밋을 마친 뒤 204를 반환하고 후속 작업은 그 원장에서 이어가세요.

서버 상태를 확인하려면 접수·중복·충돌·거부 건수, 저장 지연, 본문 크기, 원본 누락, 공간 구간의 누락·표본 부족, 시계 미상, ESTGPS 비율, `revision` 순서 역전 건수를 기록하는 것이 좋습니다. 인증 토큰은 로그나 저장 본문에 복제하지 마세요. 서버 수신 UTC와 원본 관측의 부팅 기준 시각도 구분해 기록해야 합니다.

## 11. 시험 서버와 운영 서버의 차이

선택적으로 빌드하는 `build/<OS>/tools/mavlink_telemetry_data_router_test_server`는 C로 작성한 HTTP 수신 기능을 시험하고, 받은 자료를 RAM에 유지하면서 TUI 로그로 확인하는 도구입니다.

- `POST /ingest`로 받으며 기본 TCP 포트는 21000입니다. `--plain` 옵션을 사용하면 검사를 통과한 JSON 레코드를 표준출력으로 내보냅니다.
- 온습도·공간 형식, 하트비트 시험 형식, `mavlink_router_status_v1` 상태 보고를 받습니다. 상태는 `statuses`, 측정·하트비트 레코드는 `records`로 따로 집계합니다. 일반 `schema:3`, 혼합 요청, CSV용 `schema:2`까지 처리하는 운영 서버는 아닙니다.
- 기본 TCP 포트+1에서 UDP도 받습니다. 예를 들어 HTTP 21000이면 UDP 21001이며, `--udp-port N`으로 바꾸거나 `--udp-port 0`으로 끕니다. UDP 포트를 열지 못하면 HTTP는 계속 동작하고 UDP 오류를 표시합니다.
- 최근의 제한된 기록으로 HTTP·UDP 사이의 동일 레코드 화면 중복을 억제합니다. 인증, 영속 수신 원장, 데이터베이스, 영속 큐는 구현되어 있지 않습니다. 이 도구가 반환하는 204는 검사와 출력이 성공했다는 뜻이며, 재시작 후에도 자료가 남는다는 뜻은 아닙니다.
- 허용 상한은 헤더 8,192B, 본문 524,288B, JSON 32,768토큰, 중첩 깊이 32, 레코드 64개입니다. 연결당 전체 시간 제한은 5초이며, 본문 상한은 수집기의 최대 송신 크기보다 작습니다.
- `Content-Length`가 필요하며 chunked `Transfer-Encoding`과 중복 `Content-Length`는 거부합니다. HTTP/1.0·1.1과 `Expect: 100-continue`는 처리합니다.
- 출력 전에 요청 전체를 검사합니다. 다만 출력 중 실패하면 앞의 레코드는 이미 출력된 상태에서 500을 반환할 수 있으므로, 영속 저장 트랜잭션의 원자성을 구현한 예제로 사용해서는 안 됩니다.

시험 서버로 온습도만 보내려면 수집기에 `--test-weather` 옵션을 지정할 수 있습니다. 일반 모드에서도 출력이 온습도만이라면 시험 서버가 수락합니다. 이 옵션만 사용하면 서버 URL이 기본21000번 포트로 바뀝니다. 다른 주소를 시험하려면 **`--server-url`도 함께 지정**해야 합니다. 설정에 `status_interval_ms=10000`을 추가하면 센서의 새 표본 유무와 별도로10초마다 기기 상태도 전송합니다.

```powershell
.\build\windows-x64\tools\mavlink_telemetry_data_router_test_server.exe --bind 127.0.0.1 --port 21000
.\build\windows-x64\mavlink_telemetry_data_router_interactive.exe --config .\telemetry.ini --test-weather --server-url http://127.0.0.1:21000/ingest
```

실물 센서를 수집하는 프로그램이나 시험 서버가 이미 실행 중이라면 합성 시험은 다른 임시 포트에서 진행하세요. 합성 `.json` 예제에는 실제 관측 자료가 들어 있지 않습니다.

예를 들어 다음 명령으로 21001번 포트에 별도 시험 서버를 실행할 수 있습니다. 이어서 저장소 루트에서 다른 PowerShell 창을 열고 합성 요청을 보내세요. 이 예시는 `--port 21001` 서버를 사용하므로 21000번 포트의 실물 관측 로그와 섞이지 않습니다.

```powershell
.\build\windows-x64\tools\mavlink_telemetry_data_router_test_server.exe --bind 127.0.0.1 --port 21001
```

```powershell
curl.exe --silent --show-error --include --header 'Content-Type: application/json' --data-binary '@docs/api/examples/synthetic-weather-gps-spatial.json' http://127.0.0.1:21001/ingest
```

정상 결과는 204와 빈 응답 본문입니다. Linux에서는 실행 파일 이름에서 `.exe`를 빼고 `curl`을 사용하세요. 혼합 요청 예제는 모든 형식을 구현한 운영 수신 API를 시험할 때 사용합니다. 현재 C 시험 서버가 혼합 요청에 400을 반환하는 것은 정상입니다.

## 12. 서버 인수 검사

| 검사 | 기대 결과 |
|---|---|
| 정상 온습도·공간·일반·혼합 요청 | 전체 커밋 후 204 |
| 동일 개별 ID 재전송 | 원본 중복 0, 응답 204 |
| 서버 커밋 후 ACK 유실·재시도 | 중복 원장으로 재삽입 방지 |
| UDP 선도착 뒤 같은 ID의 HTTP 도착 | 관측 추가 0, 전체 HTTP 요청 확인 후 204 |
| UDP 조각 역순·중복·누락·범위 오류 | 완전한 유효 본문만 해독·저장. 누락·잘못된 본문은 폐기 |
| HTTP 무응답·TCP 메모리 한도 도달 | 기존 미확인 자료 보존, 새 TCP 거부 수 표시, 새 UDP 송신 시도 지속 |
| 재시도에 새 항목 추가/포장 변경 | 기존 ID는 중복, 새 ID만 저장 |
| partial10→partial20→complete20 | 최종 n=20, 마지막 state complete |
| revision 순서 역전·동시 요청 | 큰 revision 유지 |
| 동일 revision의 다른 원본 | 충돌 식별, 최신 통계를 임의 덮어쓰지 않음 |
| 새 키·추가 필드 | 호환 처리, 원본 의미/단위 보존 |
| null/0, n=1 분산, 64비트 경계값 | 미상과 0 구분, 정수 정밀도 유지 |
| 위치·UTC 미상, ESTGPS | 가짜 GPS/UTC 생성 없음 |
| raw_dropped>0·큐 손실 | 부분 원본과 전체 통계를 구분 |
| 인증 실패·본문 과대·저장 실패 | 204 금지, 서버 오류 식별 가능 |
| 서버 재기동 | 커밋된 수신 원장·최신 구간 보존 |

JSON Schema 검사를 통과해도 평균·분산 계산, 필드 간 집계 관계, 중복 방지, 데이터베이스의 원자성, 실제 기상 관측의 정확성까지 확인된 것은 아닙니다. 구조 검사와 함께 위 표에 있는 의미와 상태 전이도 시험해야 합니다. 특히 표본 수와 분산은 관측 통계이며, 센서의 교정 정확도나 독립 표본 수와 구분해야 합니다.

## 13. 구현 근거

| 코드 | 담당 기능 |
|---|---|
| `src/sender.c`: `http_post`, `sender`, `seal*`, `encode*` | HTTP 204·재시도·큐·요청 객체 인코딩·내용 확정 시점의 메타데이터 |
| `src/udp_backup.c`, `src/udp_backup.h` | UDP 헤더·분할 송신·검증·재조립·만료 |
| `src/weather.c`: `sample_json`, `weather_batch_json` | 온습도 원본/대표값/시간 통계 |
| `src/spatial.c`: `sp_add`, `sp_take_snapshot`, `sp_batch_json` | 거리 구간과 누적 revision |
| `src/telemetry.c`: `tb_batch_json_mode` | 일반 배열 17열·통계 |
| `src/time_sync.c`, `src/time_sync.h` | boot/UTC 정렬 모델·가정 |
| `src/test_server.c`: `validate_body`, `weather_body_record`, `spatial_record` | 선택 시험 서버의 실제 수락 범위 |
| `src/tests/test_spatial_router.py` | 합성 속도별 구간·독립 통계·재시도/서버 검사 |

기존 공간 집계와 서버 규격의 시험 범위는 [검증 기록](REVIEW.md), 0.11.0의 배포 변경점은 [릴리스 노트](releases/v0.11.0.md)에 정리했습니다. 운영 서버 연동과 저장 자료의 복구 여부는 서버를 구현한 뒤 인수 검사에서 확인해야 합니다.
