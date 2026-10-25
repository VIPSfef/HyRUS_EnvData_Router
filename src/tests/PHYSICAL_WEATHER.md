# 실물 SHTC3 온습도 경로 수동 검사

실제 센서 → PX4 `SENS_ATMOS` → Windows 수집기 → TCP 21000 C 시험 서버를 개발자가 다시 검사하기 위한 도구입니다. 일반 회귀 시험과 달리 **실물 COM 포트를 점유하므로 CTest에 자동 등록하지 않습니다.** Python 표준 라이브러리만 사용하며 추가 패키지는 필요하지 않습니다.

## 실행 전

- Windows x64 빌드의 headless와 `tools/mavlink_telemetry_data_router_test_server.exe`가 필요합니다. 시험 서버는 `BUILD_TEST_SERVER=ON`으로 빌드합니다.
- PX4의 실제 SHTC3 드라이버와 `SENS_ATMOS` 송신을 별도로 준비합니다. 이 스크립트는 펌웨어 설치·보드 재부팅·시동 변경·PX4 콘솔 명령을 실행하지 않습니다.
- QGC 등 다른 COM 소유자가 포트를 해제한 상태여야 합니다. TCP 21000의 다른 서버도 종료해야 합니다. 다른 프로세스를 찾아 강제로 종료하지 않습니다.
- 실제 관측값은 RAM에서만 처리합니다. **실제 관측값이 들어 있는 자식 프로그램 stdout·TUI 프레임을 파일로 리다이렉트하거나 로그·화면 캡처로 저장하지 마세요.** 아래 도구는 최종 상태·개수만 출력합니다. 임시 파일은 접속 설정 INI만 만들고 종료 시 지웁니다.

옵션 없는 실행은 계획만 표시하며 COM 포트와 서버를 열지 않습니다.

```powershell
python src/tests/physical_weather_check.py
python src/tests/physical_weather_tui_check.py
```

## 원본 JSON과 통계 검사

다음은 GPS·UTC가 없는 실내 조건의 예입니다. 위치 정보가 제공되는 조건에서는 `--position unknown --clock-model unknown`을 생략하거나 조건에 맞춰 선택합니다.

```powershell
python src/tests/physical_weather_check.py --run --serial COM5 --duration-ms 12000 --position unknown --clock-model unknown
```

`--run`을 명시한 경우에만 실제 C 서버와 수집기를 자식으로 실행합니다. COM5 고정 입력·자동 탐색 OFF·UDP OFF·1초 전송을 사용합니다. 6~20초의 유한 시험이며 종료 시 자신이 시작한 자식만 정리합니다.

원본은 C 서버 stdout에서 RAM으로 받아 다음을 독립 검사합니다.

- 공식 메시지 8009, 온도 K·상대습도 비율의 SI 배율, 존재하지 않는 pressure 항목을 생성하지 않음.
- 측정 boot 시각, 같은 시각의 온습도 쌍, 실제 원본 중 대표 표본, 평균·최솟값·최댓값·표본분산(`n-1`).
- 위치·속도·UTC·불확도가 없을 때 `null`과 품질 비트가 일치함. 위치 미상을 0 좌표로 채우지 않음.
- 서버의 검증·HTTP 204 수락 수, 중복된 확인 완료 배치 없음, 종료 시 손실 및 보류 큐 0.
- 서버 stdout에서 관찰한 요청 간격이 설정한 1초 동작과 맞는지 확인. 이는 네트워크 전송 시작의 정밀 계측은 아님.

온도와 습도 콜백 사이에 배치가 나뉘면 인접 배치 전체에서 측정시각으로 짝을 맞춥니다. 센서 위치를 모르더라도 온습도 관측을 유실로 처리하지 않습니다. 각 캡처 버퍼 상한은 8 MiB이며, 초과하면 실패합니다. JSON 분석 객체와 Python 런타임의 메모리는 별도입니다.

## 실제 C 서버 TUI 검사

원본 통계 검사가 끝난 뒤 별도로 실행합니다.

```powershell
python src/tests/physical_weather_tui_check.py --run --serial COM5 --duration-ms 12000 --position unknown
```

`physical_weather_conpty.py`가 Windows 가상 콘솔을 만들어 실제 C 서버의 ANSI TUI 프레임을 RAM에서 받습니다. 온습도·SI 라벨·평균/분산·측정시각·위치 미상 표시와 수락/큐 상태를 검사합니다. 창을 띄우거나 화면 파일을 저장하지 않습니다. 원본 통계 계산은 앞의 JSON 검사 역할이며, 이 검사는 실제 TUI 표시 내용 확인입니다.

두 검사 모두 실물 관측의 정확도·센서 교정·전원 품질·비행 안전성을 보증하지 않습니다. 다른 입력 포트나 서버를 자동 탐색하지 않으며, 프로그램 종료 후 COM 포트를 다시 사용할 수 있습니다.
