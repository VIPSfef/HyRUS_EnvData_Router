# 합성 예제 안내

이 폴더의 JSON 파일은 **규격 검증을 위해 직접 정한 합성 값**으로 만들었습니다. 실제 센서에서 수집한 관측 데이터나 위치 기록은 아닙니다.

관측 예제 3개는 0.10.0의 실제 C 함수인 `encode_weather`, `encode_bundle`, `weather_batch_json`, `sp_batch_json`, `tb_batch_json_mode`, `tc_anchor`/`tc_estimate`를 임시 시험 프로그램에서 호출해 생성했습니다. 생성 후에는 읽기 쉽도록 JSON 들여쓰기만 바꾸었습니다. 상태 예제는 `sr_encode`와 시험 서버의 계약에 맞춰 직접 정한 합성 값입니다. 생성 과정에서 하드웨어, MAVLink 소켓, HTTP는 사용하지 않았습니다.

| 예제 | 확인할 내용 |
|---|---|
| [시각과 위치를 모르는 온습도 데이터](synthetic-weather-unknown.json) | 공식 HYGROMETER_SENSOR 형태의 값만 보존한 경우입니다. 시각과 위치는 null이며, 공간 설정은 비활성 상태입니다. |
| [GPS 위치와 공간 partial이 있는 데이터](synthetic-weather-gps-spatial.json) | SI 단위의 온도와 습도 표본이 각각 1개씩 있습니다. 속도의 유효성을 명시하며, 누적 partial 2개와 n이 1일 때의 분산 null을 보여 줍니다. |
| [일반 텔레메트리와 온습도를 함께 묶은 데이터](synthetic-mixed-bundle.json) | 완전한 weather envelope와 numeric3 envelope를 함께 보냅니다. stats, 17열 raw, 원시 시계 앵커가 포함되어 있습니다. 압력의 hPa는 일반 경로에서 사용하는 표시 단위이며, SI 단위로 변환된 값으로 가정하면 안 됩니다. |
| [기기 하트비트가 끊긴 상태 보고](synthetic-status-disconnected.json) | GCS 하트비트가 살아 있어도 선택한 기기의 연결은 false입니다. 마지막 온도는 STALE, 습도는 UNKNOWN이며 sample_count=0입니다. 프로세스 실행 시간과 각 수신 경과 시간을 구분합니다. |

예제의 위도, 경도, 측정값은 규격을 검증하기 위한 값입니다. 실제 관측 데이터는 이 폴더에 복사하지 마세요.
