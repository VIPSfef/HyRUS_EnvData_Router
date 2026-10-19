# PX4 온습도 시험 패치

이 디렉터리는 수집기의 코드를 대신하는 펌웨어가 아니라 **PX4 SITL에서 재현 가능한 관측값을 만드는 선택 패치**입니다. 실제 센서에서 나온 표준 `HYGROMETER_SENSOR`의 온습도 해독에는 이 패치가 필요 없습니다. 여기서는 로컬 PX4의 습도 배율을 교정하고, 표준 메시지에 없는 측정시각을 로컬 MAVLink 2 확장으로 보존합니다.

## 검증한 기준 버전

| 저장소 | 커밋 |
|---|---|
| PX4-Autopilot | `ebf8b69c66fb30242bd9d6aa5f9ea9effcbe7e46` |
| `src/modules/mavlink/mavlink` 서브모듈 | `1b1e896aa3be0d591166e086e6b9d8613bd03716` |

대상은 `px4_sitl_default`, 실행 환경은 Ubuntu 24.04 WSL입니다. 다른 버전에는 먼저 `git apply --check`로 적용 가능 여부를 확인하고 수정된 줄의 의미도 검토하세요. 기체에 업로드하거나 실제 SHT3x 하드웨어를 시험한 패치가 아닙니다.

## 적용과 빌드

아래 `ROUTER`에는 이 저장소의 **WSL 경로**를 넣습니다. 예시의 `<...>`는 실제 값으로 바꾸세요. 수정하지 않은 기준 버전에서 실행하는 순서입니다.

```sh
cd ~/PX4-Autopilot
ROUTER='/mnt/c/<수집기 저장소 경로>'
git rev-parse HEAD
git -C src/modules/mavlink/mavlink rev-parse HEAD
git apply --check "$ROUTER/support/px4/px4-hygrometer.patch"
git -C src/modules/mavlink/mavlink apply --check "$ROUTER/support/px4/mavlink-hygrometer-extensions.patch"
git apply "$ROUTER/support/px4/px4-hygrometer.patch"
git -C src/modules/mavlink/mavlink apply "$ROUTER/support/px4/mavlink-hygrometer-extensions.patch"
# 현재 빌드의 XML include 의존성이 common.xml 변경을 놓치므로 재생성을 유도합니다.
touch src/modules/mavlink/mavlink/message_definitions/v1.0/uAvionix.xml
touch src/modules/mavlink/mavlink/message_definitions/v1.0/development.xml
make px4_sitl_default -j4
```

패치 적용 전후의 변경은 PX4 본체와 MAVLink 서브모듈에서 각각 Git으로 확인할 수 있습니다. 빌드해도 **이미 실행 중인 PX4 프로세스는 바뀌지 않습니다.** 변경한 실행 파일로 PX4를 다시 시작해야 합니다. 새 SITL을 시작하는 일반적인 예는 `make px4_sitl gz_x500`입니다. 기존 Gazebo 월드와 모델을 재사용하는 실행 환경은 그 환경의 `PX4_GZ_STANDALONE`·`PX4_GZ_MODEL_NAME` 설정을 유지하세요.

현재 작업 PC의 PX4 경로는 `/home/wsl/PX4-Autopilot`입니다. PowerShell에서도 별도 SSH 로그인 없이 `wsl -d Ubuntu-24.04 --cd /home/wsl/PX4-Autopilot -- make px4_sitl_default -j4`로 빌드를 호출할 수 있습니다.

## 가상 센서 시작

새 빌드로 실행 중인 PX4와 **같은 WSL·사용자**의 다른 터미널에서 실행합니다.

```sh
cd ~/PX4-Autopilot
build/px4_sitl_default/bin/px4-mavlink stream -u 14580 -s HYGROMETER_SENSOR -r 10
build/px4_sitl_default/bin/px4-px4_mavlink_debug hygrometer 1200
```

첫 명령은 기본 API/Offboard 링크의 로컬 포트 `14580`에 온습도 스트림을 10 Hz로 설정합니다. 목적지는 기존 `14540` 그대로이며 QGC의 `14550`을 바꾸지 않습니다. 여러 PX4 인스턴스나 사용자 설정 링크에서는 해당 로컬 포트를 확인하세요.

두 번째 명령은 센서 두 개를 각각 10 Hz로 120초 동안 발행합니다. 인수를 생략하면 600표본, 약 60초이며, 허용 범위는 `1..36000`표본입니다. 자동으로 시작하거나 무한 실행하지 않습니다. 센서 인스턴스가 늘어나지 않도록 앞의 실행이 끝난 뒤 다음 명령을 실행하세요.

| 센서 | device_id | 온도 | 상대습도 |
|---|---|---|---|
| A | `0x7f000001` | `20 + 2·sin(2πt/60)` °C | `40 + 5·sin(2πt/60)` % |
| B | `0x7f000002` | `30 + 2·sin(2πt/60)` °C | `60 + 5·sin(2πt/60)` % |

샘플의 온도·습도는 같은 실제 PX4 부팅 시각을 공유합니다. 1 µs는 이 **가상 측정 시계의 양자화**를 나타내며 실제 센서의 지연·정확도 보증이 아닙니다. 실제 SHT3x 경로는 변환 구간의 불확도를 검증하지 않았으므로 `UINT32_MAX`(미상)를 보냅니다.

Linux 수집기와 시험 서버도 같은 WSL 안에서 실행해야 현재 loopback `14540`을 직접 받을 수 있습니다. 수집기는 `--test-weather`, 서버는 기본 TCP `21000`을 사용합니다. 자세한 명령은 [README](../../README.md#온습도-관측을-시험하기)를 참조하세요.

## 표준과 로컬 확장 구분

공식 HYGROMETER_SENSOR의 본문은 5 B, CRC_EXTRA는 20입니다. 표준의 온도는 0.01°C, 습도는 0.01% 단위입니다. 해당 PX4 기준 버전의 SHT3x는 %를 발행하지만 송신기가 100배를 하지 않아, 이 패치에서 `roundf(humidity * 100.f)`로 교정했습니다. 수집기에서 값 크기를 보고 배율을 추측하지 않습니다.

추가한 아래 필드는 **공식 common.xml 표준이 아닌 이 프로젝트의 로컬 MAVLink 2 확장**입니다. 공식 메시지에 측정시각이 원래 있다는 뜻이 아닙니다. 기존 5 B만 보내는 장치도 수집하지만 측정시각과 그 시각의 위치는 미상으로 보존합니다.

| 본문 offset | 로컬 필드 | 미상 |
|---|---|---|
| 5 | `uint64_t time_boot_us` | 0 |
| 13 | `uint32_t device_id` | 0 |
| 17 | `uint32_t time_uncertainty_us` | UINT32_MAX |

확장된 최대 길이는 21 B입니다. MAVLink 2의 끝 0바이트 생략으로 실제 패킷은 더 짧을 수 있으며, 1 µs인 시험 패킷은 18 B로 확인했습니다. `<extensions/>` 뒤 필드이므로 기본 필드와 CRC_EXTRA는 유지되고, 확장을 모르는 표준 수신기는 기본 온습도만 읽을 수 있습니다. 향후 공식 확장이나 다른 로컬 dialect와 필드가 충돌하지 않도록 배포할 송수신기의 XML 버전을 함께 관리해야 합니다. 범용 제품용으로 확정된 공식 확장으로 취급하지 마세요.
