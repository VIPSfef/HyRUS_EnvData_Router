# PX4 SHTC3 드라이버·공식 SENS_ATMOS 오버레이

> **현재 실물 Pixhawk 6C의 PX4 1.16.0:** [SHTC3 실측·SENS_ATMOS 송신 구성](pixhawk6c-v1.16-weather/README.md)을 사용하세요. [제품 ID·CRC 전용 진단판](pixhawk6c-v1.16-diagnostic/README.md)은 따로 보존합니다. 아래의 기본 통합 구성은 다른 PX4 기준 커밋을 대상으로 하므로 기존 1.16.0 보드에 섞어 적용하지 않습니다.

이 오버레이는 **SHTC3용 PX4 드라이버**와 **온습도 센서 한 개를 공식 `SENS_ATMOS`로 보내는 경로**를 제공합니다. ASLUAV dialect의 기존 정의를 사용하고 `common.xml`에 사설 필드를 추가하지 않습니다. 가상 센서 시험도 포함합니다. 실물 SHTC3의 제품 ID·CRC 통신은 별도 PX4 1.16.0 진단판에서 확인했으며, 실물 온습도 측정과 서버 전송은 아직 검증하지 않았습니다.

## 기준 버전과 패치

| 저장소 | 기준 커밋 |
|---|---|
| PX4-Autopilot | `ebf8b69c66fb30242bd9d6aa5f9ea9effcbe7e46` |
| `src/modules/mavlink/mavlink` 서브모듈 | `1b1e896aa3be0d591166e086e6b9d8613bd03716` |

- `px4-sens-atmos.patch`: 공식 stream·PX4 등록·wrapper 선택, API 링크 SENS10 Hz/HYG중지, 단일 가상 센서 예제, 기존 HYG 습도 배율 교정.
- `mavlink-weather-dialect.patch`: `development.xml`과 `ASLUAV.xml`을 포함하는 `weather.xml` wrapper 파일 하나 추가. 공식 메시지 정의와 `common.xml`은 바꾸지 않습니다.
- `px4-shtc3-registration.patch`: 센서 device type과 기본 OFF인 외부 I²C 시작 파라미터 등록.
- `px4-shtc3-sitl-validation.patch`: 네이티브 SITL에서 실제 어댑터를 빌드·검사하기 위한 Linux host bus1 구성. **실물 Pixhawk에는 적용하지 않습니다.**
- `src/px4/shtc3/`: 프로젝트가 소유하는 드라이버·상태 머신·CRC·모듈 설정6개 파일.

대상은 `px4_sitl_default`, 실행 환경은 Ubuntu24.04 WSL입니다. 다른 버전은 먼저 `git apply --check`로 적용 여부를 확인하고 수정 줄을 검토하세요. v0.7.0의 사설 HYG 확장 패치와 함께 적용하지 않습니다. 이미 이전 패치를 적용했다면 그 패치를 정확히 되돌리거나 별도 깨끗한 체크아웃을 준비하세요. 개인 변경을 덮어쓰는 reset 명령은 필요하지 않습니다.

## 먼저 검사하고 적용하기

Python3와 Git을 사용합니다. 명령은 수집기 저장소 기준이며 `PX4`는 실제 PX4 체크아웃 경로입니다. 기본 프로필 `sitl`은 위4개 패치와6개 드라이버 파일을 함께 준비합니다.

```sh
PX4="$HOME/PX4-Autopilot"
python3 support/px4/apply_overlay.py --px4 "$PX4" --check
python3 support/px4/apply_overlay.py --px4 "$PX4" --apply
make -C "$PX4" px4_sitl_default -j4
```

`--check` 또는 옵션을 생략한 실행은 실제 PX4 파일을 바꾸지 않습니다. 기준 커밋·SHA256을 검사하고 임시 사본에 전체 패치를 먼저 적용합니다. 같은 파일과 패치가 이미 적용됐으면 재실행해도 변경0개입니다. 다른 내용의 모듈 파일이나 충돌하는 패치는 덮어쓰지 않습니다. `--apply`의 파일 쓰기가 실패하면 이미 쓴 파일을 복원합니다. 실행 중 소스 파일을 동시에 편집하지 마세요. 이 도구는 git pull/reset·빌드·펌웨어 업로드를 자동 실행하지 않습니다.

실물 보드용 **새 체크아웃**에서는 Linux host bus 구성을 제외합니다.

```sh
python3 support/px4/apply_overlay.py --px4 "$PX4" --profile hardware --check
python3 support/px4/apply_overlay.py --px4 "$PX4" --profile hardware --apply
```

그다음 실제 기체 보드 설정에서 `CONFIG_DRIVERS_HYGROMETER_SHTC3=y`와 `CONFIG_MAVLINK_DIALECT="weather"`를 선택하고 해당 보드 타깃을 빌드합니다. 기존 보드의 I²C 구성을 사용하며 Linux bus1을 물리 포트로 간주하지 않습니다. 프로필 변경은 이미 적용한 패치를 되돌리는 명령이 아니므로 SITL용 체크아웃을 그대로 실물용으로 전환하지 않습니다.

두 프로필의 공통 SENS 패치는 검증한 SITL의 기본 dialect도weather로 설정합니다. 다른 보드의 설정까지 자동으로 찾아 바꾸지는 않습니다. 물리 보드 종류·포트가 정해지지 않은 상태에서 특정 기체 펌웨어를 빌드·업로드했다고 주장하지 않습니다.

dialect가 바뀌므로 이전 생성 헤더가 남은 빌드는 깨끗한 빌드 트리에서 다시 구성하는 것이 확실합니다. 기존 `build/px4_sitl_default`에 rootfs·파라미터·dataman 등 실행 상태가 있으면 **전체 폴더를 별도 이름으로 백업하고** 새 빌드를 만드세요. 기존 프로세스가 그 폴더를 사용하는 중에는 먼저 정상 종료해야 합니다. 필요한 실행 상태 복원은 PX4 실행 환경에 맞춰 선택하고, 상태 파일을 무조건 삭제하지 마세요.

빌드가 성공해도 이미 실행 중인 PX4 프로세스는 바뀌지 않습니다. 새 실행 파일로 다시 시작해야 합니다. 새 SITL 실행 예는 `make px4_sitl gz_x500`이고, 기존 Gazebo 월드·모델을 재사용한다면 해당 환경의 `PX4_GZ_STANDALONE`·`PX4_GZ_MODEL_NAME` 설정을 유지합니다. 이 작업 PC의 소스 경로는 `/home/wsl/PX4-Autopilot`입니다. PowerShell에서도 별도 SSH 로그인 없이 다음처럼 호출할 수 있습니다.

```powershell
wsl -d Ubuntu-24.04 --cd /home/wsl/PX4-Autopilot -- make px4_sitl_default -j4
```

## 실제 SHTC3 모듈 사용

SHTC3는 기존 SHT3x와 명령·주소가 다른 별도 센서입니다. 칩 전원1.62..3.6 V, 보통3.3 V를 사용하며 I²C 주소는고정0x70입니다. Pixhawk 포트의 VCC가5 V일 수 있으므로 모듈의 레귤레이터·레벨변환·풀업과 보드 자료를 확인해야 합니다. 칩에5 V를 직접 공급하는 배선을 뜻하지 않습니다. [제조사 데이터시트](https://sensirion.com/media/documents/643F9C8E/63A5A436/Datasheet_SHTC3.pdf)

드라이버를 포함해 빌드한 PX4 콘솔에서 외부 I²C를 선택합니다. 필요하면 `-b`에 실제 보드 버스를 지정합니다. 기본 버스 속도400 kHz, 주소0x70이며 다른 주소를 지정하면 거부합니다.

```sh
shtc3 start -X
shtc3 status
shtc3 stop
```

기체를 켤 때 자동으로 시작하려면 `SENS_EN_SHTC3=1`을 저장하고 재시작합니다. 기본값0은자동시작하지 않습니다. `status`의 제품ID 확인은 SHTC3 계열 식별이며 칩 고유 일련번호나 교정 인증이 아닙니다. uORB의device_id도버스·주소·드라이버 종류를 나타내는 식별값이며 SENS_ATMOS wire에는 실리지 않습니다.

변환 완료 대기는 상태 머신과 work queue의 지연 스케줄로 처리합니다. 약13 ms 디지털 변환 대기와 공칭 습도응답8초는 다르며 이동 관측의 센서 지연을 자동 보정하지 않습니다. 실제 전원·통신·CRC·측정 지연·교정은 센서가 연결된 후 별도 확인해야 합니다. 현재 결과는 컴파일과 fake-bus 시험, 가상 센서 송수신을 구분해 기록합니다.

## PX4 업스트림을 업데이트할 때

1. 기존 기체 설정·실행 상태를 보존하고 **새 PX4 체크아웃**을 준비합니다. dirty checkout에서 자동 pull하거나 강제로 덮어쓰지 않습니다.
2. 새 버전은 `--check --allow-other-revision`으로 패치 문맥을 검사합니다. 적용할 프로필도같이 지정합니다.
3. 충돌하면 해당 작은 패치를 검토해 갱신합니다. driver 모듈·SENS stream·SITL host bus 패치를 분리했으므로 관련 층만 수정합니다. device type의로컬값0xB9도업스트림 할당과 충돌하지 않는지 확인합니다.
4. 명시적 `--apply --allow-other-revision`으로 **새 체크아웃에** 적용하고 PX4 타깃 빌드·드라이버 fake-bus 검사·SITL→TCP 시험을 다시 수행합니다. 문맥 검사만으로 API·빌드·하드웨어 호환성을 확인한 것은 아닙니다.
5. 검증한 PX4/MAVLink 커밋과 프로젝트 소스·패치 SHA256을 `overlay-manifest.json`에 갱신해 Git으로 함께 관리합니다. 실제 센서를 사용할 때는 하드웨어 시험도 별도 수행합니다.

프로젝트 소스를 수정한 유지보수자가 파일 해시만 다시 계산하는 예입니다. 검증한 기준 커밋은 검토 후별도 변경하세요.

```sh
python3 - <<'PY'
import hashlib, json
from pathlib import Path
p = Path('support/px4/overlay-manifest.json')
m = json.loads(p.read_text())
for item in m['patches'] + m['overlays']:
    item['sha256'] = hashlib.sha256(Path(item['source']).read_bytes()).hexdigest()
p.write_text(json.dumps(m, indent=2) + '\n')
PY
```

적용 도구 회귀 검사는 실제 PX4 대신 임시 Git 저장소에서 실행합니다.

```sh
python3 support/px4/test_apply_overlay.py
```

## 센서 한 개 시작

변경한 PX4가 실행 중인 **같은 WSL·사용자**의 다른 터미널에서 실행합니다.

```sh
cd ~/PX4-Autopilot
build/px4_sitl_default/bin/px4-px4_mavlink_debug hygrometer 1200
```

인수1200은10 Hz로 약120초입니다. 생략하면600표본(약60초), 허용 범위는1..36000표본이며 자동으로 시작하거나 무한 실행하지 않습니다. **앞의 실행이 끝난 뒤 다음 시험을 시작**하세요. 같은 명령을 겹쳐 실행하면 거부하며, uORB instance0이 다른 센서에 점유되어 있어도 별도 인스턴스를 만들지 않습니다.

센서 값은 `20 + 2·sin(2πt/60)` °C와 `40 + 5·sin(2πt/60)` %RH입니다. 하나의 가상 센서에서 같은 PX4 부팅 시각으로 온도와 습도를 발행합니다. 디버그 예제의 원래 모드와 구분된 명시적 SITL 시험 기능입니다. 실제 기상 센서 모델이나 외부 환경을 시뮬레이션한 정확도 검증은 아닙니다.

API/Offboard 링크에는 시작 스크립트가 HYGROMETER_SENSOR를 중지하고 SENS_ATMOS를10 Hz로 활성화합니다. 기본 경로는 PX4 로컬14580→수집기14540이고 QGC의14550은 유지합니다. 기존 세션에서 직접 같은 stream을 설정하려면 다음 명령을 사용할 수 있습니다. 여러 PX4 인스턴스에서는 해당 로컬 포트를 확인하세요.

```sh
build/px4_sitl_default/bin/px4-mavlink stream -u 14580 -s HYGROMETER_SENSOR -r 0
build/px4_sitl_default/bin/px4-mavlink stream -u 14580 -s SENS_ATMOS -r 10
```

현재 WSL loopback14540을 받으려면 서버와 Linux 수집기를 같은 WSL에서 실행합니다. 수집기 `--test-weather`, 서버 TCP21000의 명령은 [메인 README](../../README.md#온습도-관측을-시험하기)에 있습니다. Windows 수집기가 WSL 내부 loopback을 자동으로 받을 수 있다는 뜻은 아닙니다.

## wire 규격과 불명확한 정보

공식 SENS_ATMOS(ID8009, CRC_EXTRA144)의 본문은 다음16 B입니다. 메시지 ID가255보다 크므로 MAVLink2를 사용합니다. unsigned 패킷 최대28 B이며 끝0바이트는 생략될 수 있습니다.

| offset | 필드 | 단위 |
|---|---|---|
| 0 | uint64 timestamp | 부팅 기준 µs |
| 8 | float TempAmbient | °C |
| 12 | float Humidity | %RH |

**온도와 습도 float를 그대로 보내며 습도에100을 곱하지 않습니다.** 예를 들어25.0°C와65.0%를25.0,65.0으로 보내고 수집기에서298.15 K,0.65로 바꿉니다. HYGROMETER_SENSOR의0.01% 정수 배율과 다릅니다.

SENS_ATMOS에는 센서ID·instance·시각오차·측정분산 필드가 없습니다. 수집기의 sensor0/instance0은 source와 메시지 종류에 속한 논리적 단일 관측이고 실제 칩 식별값이 아닙니다. 없는 오차는 null로 남깁니다. timestamp_sample의µs 자료형을 근거로1µs 정확도를 보증하지 않습니다. 실제 SHT3x 드라이버는 별도 지원 경로이며 변환 지연·교정·센서가 실제 획득한 시각은 하드웨어에서 검증해야 합니다.

기존 HYGROMETER_SENSOR는 공식5 B 온습도 값과id만 수집합니다. v0.7.0의 사설 timestamp/device/uncertainty 확장 바이트는 더 이상 해석하지 않습니다. 수집기는 메시지 ID별 그룹을 분리하므로 두 공식 메시지를 수동으로 함께 보내도 통계를 합치지 않습니다. 동일 물리 관측을 두 번 센 것인지는 표준 필드만으로 판단할 수 없으므로 시험에서는 SENS_ATMOS 하나만 활성화합니다.

공식 정의: [SENS_ATMOS](https://mavlink.io/en/messages/ASLUAV.html#SENS_ATMOS), [ASLUAV.xml](https://github.com/mavlink/mavlink/blob/master/message_definitions/v1.0/ASLUAV.xml).
