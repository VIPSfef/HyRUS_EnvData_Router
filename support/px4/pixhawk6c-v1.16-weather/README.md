# Pixhawk 6C · PX4 v1.16.0 SHTC3 실측 전송

이 구성은 **SHTC3 → PX4 → 공식 MAVLink `SENS_ATMOS` → 수집기 → TCP 서버** 경로를 위한 실물 보드용 소스입니다. 앞선 제품 ID 확인용 진단판을 별도로 보존하며, 이 구성에 SITL 가상 센서나 Linux I²C 시험 버스는 넣지 않습니다.

**2026-10-25에 승인받은 최종 개선판을 실물 Pixhawk 6C에 설치하고, I²C2 SHTC3 → USB MAVLink 2 SENS_ATMOS → Windows headless 수집기 → 실제 C 시험 서버 TCP 21000 전송을 확인했습니다.** 서버의 plain JSON 출력과 실제 TUI 표시에서 각각 HTTP 204 수락 12회를 확인했고 종료 시 pending은 0이었습니다. GPS fix가 없어 위치·UTC는 null로 유지했으며, 비행 중 공간 복원이나 센서 교정 정확도를 검증한 시험은 아닙니다.

## 버전과 변경 범위

- PX4: `6ea3539157ca358c70a515878b77077af7d4611d` (`v1.16.0`), 타깃 `px4_fmu-v6c_default`.
- MAVLink: `5bfd76d80281f6027134e854aafe6cb3dbfbe9e1`. 공식 `common.xml`, `development.xml`, `ASLUAV.xml`을 수정하지 않습니다.
- 공유 SHTC3 드라이버와 `shtc3_probe`를 유지하고, [송신 스트림](../../../src/px4/sens_atmos/SENS_ATMOS.hpp)과 공식 dialect 포함용 [weather.xml](../../../src/px4/sens_atmos/weather.xml)을 추가합니다.
- 기존 비행 기능·센서·시리얼 포트·무선 통신 파라미터를 제거하거나 변경하지 않습니다. USB/Onboard의 온습도 기본 스트림과 표준 HYG 습도 배율만 아래처럼 변경합니다.
- 이 manifest는 `hardware` 프로필 전용입니다. 기존 다른 기준 버전용 manifest나 SITL 패치를 섞지 마세요.

## 측정값과 시간

공식 `SENS_ATMOS`의 메시지 ID는 **8009**, 기본 payload는 **16바이트**입니다.

| 필드 | 보내는 값 |
|---|---|
| `timestamp` | PX4 부팅 후 측정 명령을 내리기 직전의 시각, µs |
| `TempAmbient` | 센서 온도, °C |
| `Humidity` | 상대습도, %RH |

압력·센서 ID·분산·시간 오차 필드는 이 공식 메시지에 **없습니다**. 가상의 압력값이나 사설 확장을 추가하지 않습니다. 수집기는 온도를 K, 상대습도를 0–1 비율로 바꾸고 시간·위치·통계를 묶습니다. GPS fix/UTC가 없으면 해당 값은 알 수 없는 값으로 남겨야 합니다.

`timestamp_sample`은 I²C 측정 명령 직전 시각의 근사치입니다. 버스 잠금 대기, 센서 변환 구간과 물리 응답 지연이 있어 µs 표시가 µs 정확도를 뜻하지 않습니다. 스트림은 instance 0 하나만 사용하며, 새 표본이 없거나 시각이 0·미래·1초 초과 과거인 표본, 비유한 온습도, 범위 밖 습도는 보내지 않습니다. 온도는 절대영도 아래 값을 거부합니다.

## USB와 무선 링크

`SENS_ATMOS`는 **MAVLink 2가 필요**합니다. 송신 스트림은 MAVLink 1 채널에서 대역 예약과 전송을 모두 하지 않습니다.

- `Onboard`와 USB `Config` 모드: 기본 `HYGROMETER_SENSOR` 대신 `SENS_ATMOS`를 새 uORB 표본 기준(`unlimited_rate=-1`)으로 설정합니다. 같은 값을 반복 전송하지 않으며, 실제 센서는 약 10 Hz로 측정합니다.
- `Normal`, `ExtvisionMin`, `OSD`: 기존 HYG 스트림 설정과 저속 주기를 유지합니다.
- 기존 무선 링크의 MAVLink 버전이나 속도는 자동으로 바꾸지 않습니다. MAVLink 1 링크에서는 ID 8009를 전송할 수 없습니다. HYG의 ID 12920도 MAVLink 2가 필요합니다.
- 공식 HYG 스트림의 수동 활성화 기능은 유지합니다. 같은 링크에서 HYG와 SENS를 함께 켜면 같은 센서 자료가 두 형식으로 올 수 있으므로, 수집에는 한 형식을 선택합니다. 기본 USB/Onboard 구성은 HYG를 켜지 않습니다.

기존 PX4 v1.16 HYG 습도 변환도 표준에 맞게 교정합니다. 예를 들어 uORB의 `50 %RH`를 HYG의 `5000`(0.01 %RH 단위)으로 보내며, 수집기의 `5000 × 0.0001 = 0.5`와 일치합니다. 기존 코드는 %RH를 그대로 정수화하여 표준 해석 시 약 100배 작아졌습니다. 온도는 기존과 같은 0.01°C 단위이고 사설 필드는 없습니다.

## 센서 시작과 자동 시작

공유 드라이버의 `SENS_EN_SHTC3` 기본값은 **0**입니다. 부팅 중 `rc.sensors`가 이 파라미터를 비교하므로 기본값 0에서도 사용된 파라미터로 표시되어 MAVLink 목록에 광고될 수 있습니다.

이 **Pixhawk 6C 구성의 자동 시작 버스는 물리 I²C2**로 명시했습니다. 파라미터가 1일 때만 다음 명령을 실행합니다.

```sh
shtc3 -X -b 2 start
```

현재 확인한 보드에는 `SENS_EN_SHTC3=1`을 저장했습니다. 새 보드에 이 펌웨어를 설치하면 기본값은 0이므로, 해당 버스와 센서 연결을 확인한 뒤 콘솔에서 다음과 같이 활성화합니다.

```sh
param set SENS_EN_SHTC3 1
param save
reboot
```

다른 포트나 다른 보드에 적용할 때는 실제 물리 버스를 확인한 후 해당 시작 줄을 수정해야 합니다. 전체 외부 버스를 자동 탐색하지 않습니다. 일시적인 수동 시작도 같은 명령을 사용하며, 중복 시작 시 기존 드라이버 인스턴스가 보호됩니다.

지속 드라이버 실행 중에는 같은 주소를 대상으로 `shtc3_probe`를 동시에 실행하지 마세요. 진단이 필요하면 비행·시동이 해제된 정비 상태에서 지속 드라이버를 먼저 중지합니다.

## 코드 크기와 실행 시간

센서 처리는 표본 수에 따라 누적 탐색하지 않는 O(1) 상태 기계이며, 표본마다 동적 메모리를 할당하지 않습니다. 정상 한 주기에 I²C 동작 4회와 CRC 검사 2회를 수행합니다. 13 ms 변환 대기는 공유 센서 work queue에서 잠들지 않고 다음 실행을 예약합니다.

이 보드의 기본 I²C 클록 표는 100 kHz입니다. 드라이버의 기본 400 kHz 인수는 허용 가능한 최대 속도이지 버스를 400 kHz로 바꾸는 명령이 아닙니다. 전역 버스 클록을 변경하지 않습니다. ACK를 포함한 공칭 144 SCL 클록은 100 kHz에서 약 1.44 ms, 10 Hz에서 약 1.44%의 버스 시간을 사용합니다. START/STOP·경합·스케줄링·오류 타임아웃을 포함한 최악 실행 시간은 보증하지 않습니다.

SENS 스트림도 고정 크기 표본 하나를 처리합니다. 기존 MAVLink 태스크의 각 반복에서 새 uORB generation만 확인하며, 새 표본이 있을 때만 복사·직렬화합니다. 실제 PX4 상수에 설정된 루프 대기는 1500–10000 µs이므로 지연을 제외한 이론상 반복률은 약 100–667회/초이며, 기존 반복마다 O(1) 확인 한 번을 추가할 뿐 새 태스크나 타이머는 없습니다. 스케줄링 지연이 있으면 더 느려질 수 있어 최소 100 Hz를 보장한다는 뜻은 아닙니다. 확인 한 번의 실물 CPU 시간은 아직 측정하지 않았습니다.

무서명 MAVLink 2의 16바이트 payload와 12바이트 부가 데이터를 합치면 소스 10 Hz에서 최대 280 B/s, 서명 포함 시 410 B/s입니다. 현재 관찰된 USB 할당량 100000 B/s의 0.28–0.41%입니다. PX4의 rate_mult 계산은 unlimited 스트림의 대역을 예약하지 않으므로, 이 구성은 기존 고대역 USB/Onboard에서만 기본 활성화하며 저속 모드 설정은 유지합니다.

초안의 고정 10 Hz 송신과 약 10 Hz 센서가 서로 다른 위상으로 실행되면서 1슬롯 uORB 표본이 합쳐지는 현상을 실물에서 확인해 새 표본 기준으로 바꿨습니다. 100 ms를 넘는 태스크 정체나 링크 혼잡에서도 무손실이라는 보장은 아닙니다.

## 새 PX4 체크아웃에 적용하기

소스 저장소 루트에서 실행합니다. `PX4`는 위의 정확한 버전과 서브모듈을 준비한 별도 체크아웃입니다.

```sh
PX4=/path/to/PX4-Autopilot-v1.16.0
python3 support/px4/apply_overlay.py --px4 "$PX4" --manifest support/px4/pixhawk6c-v1.16-weather/overlay-manifest.json --profile hardware --check
python3 support/px4/apply_overlay.py --px4 "$PX4" --manifest support/px4/pixhawk6c-v1.16-weather/overlay-manifest.json --profile hardware --apply
make -C "$PX4" px4_fmu-v6c_default -j6
```

첫 명령은 읽기 전용 검사입니다. 적용 도구는 소스 해시·두 저장소의 커밋·패치 문맥을 검사하고 수정된 모듈을 강제로 덮어쓰지 않습니다. Git pull/reset이나 업로드는 실행하지 않습니다. 같은 manifest를 다시 적용하면 변경 0개입니다.

## 빌드와 검증

ARM 빌드 결과는 저장소의 `build/px4-fmu-v6c-v1.16-weather/`에 보관하며 `*.px4`가 업로드 파일, `*.elf`는 심볼 확인용입니다. `weather-build-info.json`에는 이미지·소스 해시, 보드 ID, 서브모듈, 메모리 사용량과 컴파일러를 기록합니다. 실제 보드 검증 기록은 [VERIFICATION.md](VERIFICATION.md)에 구분해 남깁니다.

이 빌드의 컴파일러는 GCC 13.2.1로 공식 원본의 GCC 9.3.1과 다릅니다. 기존 비행 기능을 끄지 않고 Flash에 들어오는지 확인합니다. NuttX 코드는 기존과 같은 `886acbbdb4f061e5c0ce1a76afbcfa7cb7df9849`입니다. 새 작업 폴더의 버전 생성용 태그는 `nuttx-11.0.0`만 준비하여, 다른 최신 태그 때문에 OS 표시가 달라지는 것을 피했습니다.

향후 PX4 업데이트는 새 체크아웃에서 `--allow-other-revision --check`로 문맥을 먼저 검사하고 API·dialect·보드 설정·빌드·센서 동작을 다시 검증합니다. 원본 작업 트리를 reset하거나 다른 기준 버전의 패치를 섞지 않습니다.

참고: [공식 SENS_ATMOS 정의](https://mavlink.io/en/messages/ASLUAV.html#SENS_ATMOS), [SHTC3 데이터시트](https://sensirion.com/media/documents/643F9C8E/63A5A436/Datasheet_SHTC3.pdf).


센서의 물리 응답 지연과 연속 표본 사이의 상관이 있으므로, 1초 구간 표본분산은 그 구간 값의 산포이며 센서 정확도·교정 오차를 나타내는 정확도 분산으로 대체할 수 없습니다. [제조사 응답시간·정확도 정의](https://sensirion.com/media/documents/643F9C8E/63A5A436/Datasheet_SHTC3.pdf)
