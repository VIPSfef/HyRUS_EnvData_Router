# 서버 연동 규격과 검증 도구 · 0.10.0

이 폴더에는 수집기가 서버로 보내는 데이터를 설명하는 규격 파일과 검증 도구가 들어 있습니다. 규격은 `src/sender.c`, `weather.c`, `spatial.c`, `telemetry.c`, `time_sync.h`의 실제 출력에 맞추었습니다. 서버를 처음 구현한다면 먼저 [서버 API 문서](../SERVER_API.md)를 읽고, 이 폴더의 파일을 구현과 검증에 활용하세요.

| 파일 | 용도 |
|---|---|
| [ingestion.schema.json](ingestion.schema.json) | JSON Schema 2020-12로 작성한 데이터 구조입니다. 일반 MAVLink3, 온습도, 혼합 bundle, 하트비트 시험, 개발용 CSV2의 다섯 가지 최상위 형식을 정의합니다. |
| [openapi.json](openapi.json) | OpenAPI 3.1.1로 작성한 HTTP API 규격입니다. POST `/ingest`, 선택적으로 사용하는 bearer 인증, 수신 확인 응답 204를 정의합니다. |
| [examples](examples/README.md) | 실제 C 인코더로 만든 합성 예제 3개입니다. 실제 센서에서 수집한 관측 파일은 아닙니다. |
| [validate.py](validate.py) | 설치된 Python jsonschema를 사용해 메타스키마와 예제를 검사합니다. 잘못된 입력을 거부하는지, 경계값을 올바르게 처리하는지도 확인합니다. |

## 규격을 검증하는 방법

Ubuntu에서는 `python3 docs/api/validate.py`를 실행하면 됩니다. C 시험 서버의 응답까지 확인하려면 `--test-server build/linux-x64/tools/mavlink_telemetry_data_router_test_server`를 붙이세요.

시험 서버를 사용하는 검사는 임시로 할당한 loopback 포트에서만 실행합니다. COM, 21000, QGC 포트는 사용하지 않습니다. 필요한 도구가 없더라도 자동으로 설치하지 않으며, 자료를 외부 서버로 보내지 않습니다.

검증 도구는 OpenAPI 문서의 로컬 참조와 필수 구조도 확인합니다. 다만 OpenAPI 전용 메타검증기를 대신하지는 않습니다.

## 필드를 추가하거나 빌드 상한을 바꿀 때

스키마는 새로운 **객체 필드**를 허용합니다. 서버는 모르는 필드가 추가되었다는 이유만으로 요청 전체를 거부하지 않도록 구현하세요. 기존 필수 필드와 단위, 알려진 schema 식별자, 17열 raw tuple의 의미는 유지해야 합니다. 새로운 위치 출처나 schema 종류는 이름을 추가하는 것만으로 자동 해독할 수 없으므로, 별도의 처리 규칙이 필요합니다.

규격에 적힌 기본 빌드 상한은 다음과 같습니다.

- 일반 데이터: raw 512개, 키 16개
- 온습도 데이터: raw 128개, 그룹 16개
- 공간 데이터: snapshot 144개
- 요청에 묶는 항목: 4개

이 상한을 바꿔 빌드한다면 규격도 함께 검토해야 합니다.

## 큰 정수와 시각의 미상값을 처리할 때

64비트 정수는 현재 규격의 표현 방식을 유지해야 합니다. 일반 데이터의 batch_id, window, t_us, UTC, 누적 loss와 공간 데이터의 식별자, revision은 십진 문자열입니다. 반면 온습도의 record.seq와 하트비트의 seq, source_generation, 단조 시각은 현재 코드에서 **JSON 숫자**로 보냅니다. JavaScript Number로 큰 정수가 반올림되지 않도록 손실 없는 파서를 사용하세요.

일반 raw에서 허용하는 시각 오차는 0..20000µs이고, 항법 간격은 0..200000µs입니다. 온습도의 시각 오차와 raw_clock_anchor.uncert_us는 값을 모를 때 null로 보냅니다. 일반 raw의 UTC가 알려진 경우에는 utc_uncert_us에 uint32 값이 들어갈 수 있으며, 여기에는 미상 sentinel도 포함됩니다. UTC가 없으면 두 열 모두 null입니다.

## 서버에서 추가로 확인해야 하는 것

스키마는 구조와 형식, 일부 필드 간 조건을 검사합니다. 예를 들어 n이 1일 때 분산이 null인지, GPS의 estimation_age_us가 0인지, 위치를 모를 때 속도와 불확도가 null인지 확인합니다. field_order, 17열 tuple, decimal64 범위도 검사합니다.

다음 사항은 스키마 검사만으로 보장할 수 없습니다. 서버에서 별도의 검증과 저장 정책을 마련해야 합니다.

- n, raw_n, raw_dropped의 합계, 시각과 거리의 순서, 그룹 전체의 상한, 대표 표본이 실제 원본에 속하는지 여부
- 평균, M2, 분산의 재계산, n과 배열 길이의 일치 여부, 품질 비트의 의미, 필드 간 수치 비교
- 세션과 소스별 중복 제거, 공간 revision의 증가 확인과 최신값 교체, transaction과 영속 저장
- 좌표, 시계, 센서의 실제 정확도와 기상장을 복원할 수 있는지 여부

## 재전송과 수신 확인

전송 대기열에서 확정된 개별 항목(sealed ID)의 관측값, 원점, 시계 정보는 재시도 중에도 바뀌지 않습니다. 다만 뒤에 대기 중인 항목이 추가되거나 bundle로 묶이는 방식, 누적 loss가 바뀌면 전체 요청 내용은 달라질 수 있습니다. 따라서 요청 본문의 hash를 중복 제거 키로 사용하면 안 됩니다.

서버가 요청에 들어 있는 모든 ID를 수락한 뒤 204로 응답해야 수집기가 해당 데이터를 메모리에서 해제합니다. 200, 202, 그 밖의 응답을 받거나 연결이 끊기면 데이터는 계속 전송 대기 상태로 남습니다.

C 시험 서버는 받은 내용을 RAM에서 표시하며 통신을 확인하는 용도입니다. DB 저장, 인증, 중복 제거 기능은 제공하지 않습니다. 일반 schema3, bundle, CSV2를 처리하는 운영 서버 구현도 포함하지 않습니다.

규격의 문법은 [JSON Schema 2020-12의 tuple 규칙](https://json-schema.org/draft/2020-12/release-notes)과 [OpenAPI 3.1.1](https://spec.openapis.org/oas/v3.1.1.html)을 따릅니다.
