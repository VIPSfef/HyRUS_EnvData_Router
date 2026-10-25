# 서버 연동용 기계 판독 규약 · 0.10.0

이 폴더는 실제 `src/sender.c`, `weather.c`, `spatial.c`, `telemetry.c`와 `time_sync.h`의 출력 규약을 설명합니다. 운영 서버 구현 가이드는 [SERVER_API.md](../SERVER_API.md)를 따릅니다.

| 파일 | 용도 |
|---|---|
| [ingestion.schema.json](ingestion.schema.json) | JSON Schema 2020-12. 일반 MAVLink3, 온습도, 혼합 bundle, 하트비트 시험, 개발용 CSV2의 다섯 최상위 분기 |
| [openapi.json](openapi.json) | OpenAPI3.1.1의 POST `/ingest`·선택 bearer·204 확인 계약 |
| [examples](examples/README.md) | 실제 C 인코더로 생성한 합성 예제3개. 실물 관측 파일이 아님 |
| [validate.py](validate.py) | 설치된 Python jsonschema로 메타스키마·예제·음성/경계 회귀를 검사 |

Ubuntu에서 `python3 docs/api/validate.py`로 검사합니다. 추가로 C 시험 서버를 검사하려면 `--test-server build/linux-x64/tools/mavlink_telemetry_data_router_test_server`를 붙입니다. 이 선택 검사는 임시 loopback 포트만 사용하며 COM·21000·QGC 포트를 사용하지 않습니다. 도구가 없는 환경에서 자동 설치하거나 외부 서버로 자료를 보내지 않습니다. OpenAPI 문서의 로컬 참조·필수 구조를 함께 검사하지만, 별도 OpenAPI 전용 메타검증기를 대체하지 않습니다.

스키마는 새 **객체 필드**를 허용합니다. 기존 필수 필드, 단위, 알려진 schema 식별자와17열 raw tuple의 의미는 유지해야 합니다. 새 위치 출처나 schema 종류는 이름만 추가해 자동 해독할 수 있는 확장이 아닙니다. 기본 빌드 상한(일반 raw512·키16, 온습도 raw128·그룹16, 공간 snapshot144·요청4)을 기술하며 상한을 바꿔 빌드하면 규약도 함께 검토해야 합니다.

64비트는 표현을 바꾸지 마세요. 일반 batch_id/window/t_us/UTC와 누적 loss, 공간 식별자·revision은 십진 문자열입니다. 온습도 record.seq와 하트비트 seq/source_generation/단조 시각은 현재 코드에서 **JSON 숫자**입니다. JavaScript Number로 큰 정수를 반올림하지 말고 손실 없는 파서를 사용해야 합니다. 일반 raw의 시각 오차는0..20000µs, 항법 간격은0..200000µs입니다. 온습도 시각 오차와 raw_clock_anchor.uncert_us의 미상값은 null입니다. 일반 raw의 UTC가 알려진 경우 utc_uncert_us에는 uint32 값(미상 sentinel 포함)이 나올 수 있으며, UTC가 없으면 두 열이 모두 null입니다.

스키마가 검사하는 것은 구조·형식·일부 조건입니다. n1 분산null, GPS의 estimation_age_us0, 위치 미상일 때 속도/불확도null, field_order와17열 tuple, decimal64 범위 등을 확인합니다. 다음은 서버의 별도 검사·저장 정책이 필요합니다.

- n/raw_n/raw_dropped 합계, 시각·거리 순서, 그룹 전체 상한, 대표 표본의 실제 원본 소속
- 평균·M2·분산의 재계산, n과 배열 길이 일치, 품질 비트의 의미, 필드 간 수치 비교
- 세션/소스별 중복 제거, 공간 revision 증가와 최신값 교체, transaction/영속 저장
- 좌표·시계·센서의 실제 정확도와 기상장 복원 가능성

재시도 중 **개별 sealed ID의 관측·원점·시계**는 불변입니다. 뒤 묶음 추가·bundle 포장·누적 loss 때문에 전체 요청은 달라질 수 있으므로 body hash를 중복 키로 사용하지 않습니다. 서버가 모든 ID를 수락한 뒤204를 보내야 해제됩니다. 200·202·그 밖의 응답과 연결 단절에서는 계속 보류합니다. C 시험 서버는 RAM 표시 확인용이며 DB·인증·중복 제거·일반 schema3/bundle/CSV2의 운영 구현을 제공하지 않습니다.

규격 문법은 [JSON Schema2020-12 tuple 규칙](https://json-schema.org/draft/2020-12/release-notes)과 [OpenAPI3.1.1](https://spec.openapis.org/oas/v3.1.1.html)을 따릅니다.
