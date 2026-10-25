# 합성 예제

여기 JSON은 **검증용으로 직접 정한 합성 값**이며 실제 센서 관측이나 위치 기록이 아닙니다. 실제0.10.0 C 함수 `encode_weather`, `encode_bundle`, `weather_batch_json`, `sp_batch_json`, `tb_batch_json_mode`와 `tc_anchor`/`tc_estimate`를 임시 harness에서 호출해 생성했습니다. JSON 들여쓰기만 바꾸었습니다. 하드웨어·MAVLink 소켓·HTTP는 생성 과정에서 사용하지 않았습니다.

- [시각·위치 미상 온습도](synthetic-weather-unknown.json): 공식 HYGROMETER_SENSOR 형태의 값만 보존, 시각/위치null, 공간 설정 비활성
- [GPS·공간 partial](synthetic-weather-gps-spatial.json): SI 온도/습도 각1표본, 명시적인 속도 유효성, 누적 partial2개, n1 분산null
- [일반 텔레메트리·온습도 혼합](synthetic-mixed-bundle.json): 완전한 weather envelope와 numeric3 envelope, stats/17열 raw/원시 시계 앵커 포함. 압력의 hPa는 일반 경로의 표시 단위이며 SI 변환을 가정하지 않음

합성 위·경도와 값이 포함된 이유는 규약 검증입니다. 실물 관측을 여기에 복사하지 마세요.
