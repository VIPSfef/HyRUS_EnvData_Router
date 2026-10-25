# 개발자와 공유하고 다른 PC에서 업데이트하기

이 프로젝트는 **회사 private GitLab을 공용 원본으로 두고, 각 PC에서 독립 clone**하는 구성을 권장합니다. GitHub private 저장소도 같은 방식으로 사용할 수 있습니다. 원격 저장소를 아직 만들지 않았다면 OneDrive에 Git bundle을 전달해 다른 PC에서 이력과 변경 내용을 확인할 수 있습니다.

현재 소스 위치는 `C:\Users\pjs20\OneDrive\문서\New project 3\mavlink-server-transfer`이며 로컬 Git의 기본 브랜치는 `main`입니다. 이 가이드를 작성할 때 원격 저장소는 연결되지 않았습니다. 아래 URL과 대문자 이름은 **예시 자리표시자**이므로 실제 발급받은 저장소 주소로 바꿉니다. `prj.uaslaboratory.synology.me`의 Git 서비스 경로나 접속 URL은 이 문서에서 추정하지 않습니다.

## 개발자에게 전달할 자료

| 자료 | 용도 |
|---|---|
| [README](../README.md) | 실행 프로그램·설정·일반 사용법 |
| [기술 참조](REFERENCE.md) | HTTP 확인, JSON, SI 단위, 위치·시간·공간 구간 규약 |
| [서버 API](SERVER_API.md), [OpenAPI/JSON Schema](api/README.md) | 서버 수신·저장·중복 제거·거리 구간 갱신 계약과 합성 예제 |
| [src 헤더](../src/) | 모듈별 함수·자료형·호출 계약 |
| [검토 기록](REVIEW.md) | 실제 실행한 검사와 알려진 한계 |
| `src/`와 `CMakeLists.txt`, `CMakePresets.json` | 구현·Windows/Linux x64 빌드 |
| `src/tests/`, `.github/workflows/ci.yml`, `.gitlab-ci.yml` | 회귀 검사와 자동 빌드 구성 |
| [PX4 오버레이](../support/px4/) | 센서 드라이버·공식 MAVLink stream·기준 커밋과 적용 도구 |

공유 파일은 `build/sharing/`에서 확인합니다. 배포 시점의 목록과 SHA256을 함께 전달합니다.

- **`.bundle`**: 커밋 이력과 브랜치를 가진 Git 사본입니다. clone/fetch와 변경 비교에 사용합니다.
- **소스·문서 ZIP**: 현재 커밋의 파일을 읽는 용도입니다. `.git` 이력은 없으므로 이 ZIP 자체에 fetch할 수 없습니다.
- **SHA256/배포 목록**: 파일이 전부 전송됐는지와 어느 커밋인지 확인하는 기준입니다.
- 실행 파일이 필요한 사람에게는 `build/packages/`의 해당 OS 배포물을 별도로 전달합니다.

bundle은 커밋된 Git 객체와 ref만 담습니다. ignored build·실행 파일·로컬 운용 ini·토큰·관측 자료를 넣기 위한 형식이 아닙니다. 다만 과거에 커밋한 파일은 나중에 ignore해도 이력에 남으므로, 전달 범위는 현재 파일뿐 아니라 포함하는 Git 이력으로 확인합니다.

## 회사 GitLab 또는 GitHub로 협업하기

저장소 소유자가 **private 빈 저장소**를 만들고 개발자를 초대합니다. 기존 Git 이력을 올릴 것이므로 웹 화면에서 README·라이선스·gitignore를 새로 초기화하지 않습니다. 회사 GitLab을 쓰면 회사에서 정한 프로젝트 경로와 접근 방법을 따릅니다. [GitHub 저장소 생성](https://docs.github.com/en/repositories/creating-and-managing-repositories/creating-a-new-repository)

권한은 읽기·검토와 코드 변경을 구분합니다.

| 참여자 | GitLab의 예 | GitHub 조직 저장소의 예 |
|---|---|---|
| 소스 확인·검토 | Reporter | Read |
| 개발·작업 브랜치 push | Developer | Write |
| 병합·저장소 유지보수 | Maintainer | Maintain 또는 관리자가 지정한 권한 |

개인 GitHub 저장소는 조직의 세분화된 역할과 달리 소유자/협업자 체계입니다. 개인 저장소의 협업자 초대를 읽기 전용 권한이라고 가정하지 않습니다. 읽기·쓰기 권한을 세분화해야 한다면 GitHub 조직이나 회사 GitLab을 사용합니다. [GitLab 역할](https://docs.gitlab.com/user/permissions/), [GitHub 조직 역할](https://docs.github.com/en/organizations/managing-user-access-to-your-organizations-repositories/managing-repository-roles/repository-roles-for-an-organization), [개인 저장소 권한](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/repository-access-and-collaboration/permission-levels-for-a-personal-account-repository)

소유자가 실제 주소를 받은 뒤 현재 저장소에서 처음 한 번 실행할 명령입니다. 아래 주소는 예시입니다. Git 자격증명 관리자나 SSH 인증을 사용하고 토큰을 URL에 넣지 않습니다.

```powershell
Set-Location 'C:\Users\pjs20\OneDrive\문서\New project 3\mavlink-server-transfer'
git status --short
git remote add origin 'https://gitlab.example.com/TEAM/mavlink_telemetry_data_router.git'
git push -u origin main
```

GitHub라면 예시 주소를 `https://github.com/YOUR_ORG/mavlink_telemetry_data_router.git`처럼 실제 저장소 URL로 바꿉니다. 이 명령은 소유자가 연결·게시할 때 사용하는 절차이며 가이드 작성 과정에서 실행한 결과가 아닙니다. 이미 origin이 있다면 먼저 `git remote -v`로 확인하고 새 연결을 중복 추가하지 않습니다.

다른 PC에서는 OneDrive 밖의 개발 폴더에 clone합니다. Windows PowerShell 예:

```powershell
New-Item -ItemType Directory -Force -Path 'C:\dev' | Out-Null
git clone 'https://gitlab.example.com/TEAM/mavlink_telemetry_data_router.git' 'C:\dev\mavlink_telemetry_data_router'
Set-Location 'C:\dev\mavlink_telemetry_data_router'
git log -5 --oneline
```

개발자는 작업 브랜치에서 변경하고 PR(GitHub)/MR(GitLab)로 검토를 요청합니다. main 직접 수정 대신 이 흐름을 팀 규칙으로 삼고, 서비스와 요금제가 지원하는 보호 규칙을 설정합니다.

```powershell
git switch -c feature/describe-the-change
# 코드·문서 수정과 필요한 검사
git add -- src/changed_file.c docs/changed_document.md
git commit -m 'Describe the change'
git push -u origin feature/describe-the-change
```

마지막 두 파일 이름과 브랜치 이름은 작업에 맞게 바꿉니다. PR/MR에는 문제·변경 동작·검사 결과와 API/JSON 호환성 영향을 적습니다. [GitHub PR](https://docs.github.com/en/pull-requests/reference/pull-requests), [GitLab MR](https://docs.gitlab.com/user/project/merge_requests/)

## OneDrive만으로 다른 PC에서 확인하기

OneDrive는 **완성된 bundle 파일을 전달**하는 데 사용합니다. 같은 동기화 폴더 안의 동일한 `.git` 작업 트리를 두 PC에서 동시에 수정하지 않습니다. PC별 작업 폴더는 `C:\dev\...`처럼 OneDrive 밖에 두고, 공유 폴더의 bundle을 먼저 로컬 snapshot 폴더로 복사한 다음 읽습니다.

### 처음 받는 PC

1. OneDrive에서 해당 bundle과 SHA256 목록의 동기화가 끝났는지 확인합니다. 온라인 전용이면 먼저 다운로드합니다.
2. 배포 목록에서 **커밋이 표시된 실제 bundle 파일명**을 선택합니다. 아래 `REPLACE_WITH_ACTUAL_COMMIT`은 반드시 바꿉니다.
3. 로컬 사본의 SHA256을 전달받은 목록과 대조한 뒤 clone합니다.

```powershell
$sharedDir = Join-Path $env:OneDrive '문서\New project 3\mavlink-server-transfer\build\sharing'
$bundleName = 'mavlink-router-REPLACE_WITH_ACTUAL_COMMIT.bundle'
$snapshotDir = Join-Path $env:USERPROFILE 'Downloads\mavlink-router-snapshots'
New-Item -ItemType Directory -Force -Path $snapshotDir | Out-Null
$bundleLocal = Join-Path $snapshotDir $bundleName
Copy-Item -LiteralPath (Join-Path $sharedDir $bundleName) -Destination $bundleLocal
Get-FileHash -LiteralPath $bundleLocal -Algorithm SHA256
git bundle list-heads "$bundleLocal"
```

다른 PC의 OneDrive 폴더 구조가 다르면 `$sharedDir`을 실제 공유 폴더로 바꿉니다. 해시와 refs/heads/main의 커밋이 배포 목록과 맞으면 새 작업 폴더를 만듭니다.

```powershell
$cloneDir = 'C:\dev\mavlink_telemetry_data_router'
New-Item -ItemType Directory -Force -Path 'C:\dev' | Out-Null
git clone --origin onedrive --branch main "$bundleLocal" "$cloneDir"
if ($LASTEXITCODE -ne 0) { throw 'clone 실패: 경로와 bundle을 확인하세요.' }
git -C "$cloneDir" bundle verify "$bundleLocal"
if ($LASTEXITCODE -ne 0) { throw 'bundle 검증 실패' }
git -C "$cloneDir" log -5 --oneline
```

이 clone에는 읽기용 remote `onedrive`가 생깁니다. Git 공식 문서가 지원하는 bundle clone/fetch를 사용하며, bundle로 push할 수는 없습니다. [git bundle](https://git-scm.com/docs/git-bundle)

### 새 bundle로 업데이트하기

발행자는 새 커밋을 만들고 **새 커밋이 표시된 새 파일명**으로 bundle을 공유합니다. 받는 PC는 새 파일의 동기화 완료와 SHA256을 다시 확인하고 앞 절처럼 로컬 snapshot으로 복사합니다. 이전 bundle을 읽는 도중 덮어쓰는 방식으로 갱신하지 않습니다.

`$bundleLocal`이 새 로컬 bundle 경로를 가리키는 상태에서:

```powershell
Set-Location 'C:\dev\mavlink_telemetry_data_router'
git status --short
if (git status --porcelain) { throw '로컬 수정이 있습니다. 먼저 작업 브랜치에 커밋하거나 보관하세요.' }

git bundle verify "$bundleLocal"
if ($LASTEXITCODE -ne 0) { throw 'bundle 검증 실패' }
git remote set-url onedrive "$bundleLocal"
git fetch onedrive
if ($LASTEXITCODE -ne 0) { throw 'fetch 실패' }
git log --oneline --left-right main...onedrive/main

git switch main
if ($LASTEXITCODE -ne 0) { throw 'main 전환 실패' }
git merge --ff-only onedrive/main
if ($LASTEXITCODE -ne 0) { throw '분기된 이력입니다. 강제 덮어쓰기 없이 변경 내용을 검토하세요.' }
git log -5 --oneline
```

fetch는 받은 이력을 확인하는 단계이고 merge가 작업 파일에 적용하는 단계입니다. 로컬 main에 별도 커밋이 생겨 fast-forward가 불가능하면 여기서 멈춥니다. 로컬 작업을 없애는 reset/clean으로 해결하지 않습니다.

커밋 전 작업을 잠시 보관해야 한다면 원래 브랜치에서 다음을 선택적으로 사용합니다. `stash apply`는 저장한 사본을 자동 삭제하지 않습니다. 복원은 원래 작업 브랜치에서 하고, 충돌이 나면 내용을 확인합니다.

```powershell
git stash push --include-untracked -m 'before-shared-update'
git stash list
# 업데이트 뒤 원래 작업 브랜치로 돌아온 다음
git stash apply 'stash@{0}'
```

ignored build·운용 설정은 이 stash에 포함되지 않습니다. 필요한 로컬 파일은 별도로 유지합니다.

### Linux에서 받기

Windows와 같은 bundle을 사용할 수 있습니다. 아래 두 경로와 파일명은 실제 위치로 바꿉니다. SHA256 확인 뒤 clone하고 이후 새 snapshot에 remote를 바꿔 fetch합니다.

```sh
mkdir -p "$HOME/dev" "$HOME/Downloads/mavlink-router-snapshots"
cp "/PATH/TO/SYNCED/mavlink-router-COMMIT.bundle" "$HOME/Downloads/mavlink-router-snapshots/"
bundle_local="$HOME/Downloads/mavlink-router-snapshots/mavlink-router-COMMIT.bundle"
sha256sum "$bundle_local"
git clone --origin onedrive --branch main "$bundle_local" "$HOME/dev/mavlink_telemetry_data_router"
cd "$HOME/dev/mavlink_telemetry_data_router"
git bundle verify "$bundle_local"
```

업데이트 시 로컬 변경을 먼저 보존한 뒤 `git remote set-url onedrive "$bundle_local"`, `git fetch onedrive`, `git switch main`, `git merge --ff-only onedrive/main`을 차례로 사용합니다. 새 bundle 경로를 지정하고, 각 명령이 실패하면 다음 적용 단계로 넘어가지 않습니다.

## 새 공유 snapshot 발행하기

발행 담당자는 공유하려는 변경을 먼저 검토·커밋하고 main의 커밋을 기준으로 묶습니다. 다음은 전체 main 이력을 가진 bundle과 현재 소스·문서 ZIP을 만드는 예입니다. 새 개발자는 전체 bundle로 시작하고, 이 안내에서는 선행 커밋이 필요한 증분 bundle을 사용하지 않습니다.

```powershell
Set-Location 'C:\Users\pjs20\OneDrive\문서\New project 3\mavlink-server-transfer'
git status --short
$shareCommit = (git rev-parse --short=12 main).Trim()
$shareDir = Join-Path (Get-Location) 'build\sharing'
New-Item -ItemType Directory -Force -Path $shareDir | Out-Null
$bundleOut = Join-Path $shareDir "mavlink-router-$shareCommit.bundle"
$sourceOut = Join-Path $shareDir "mavlink-router-source-$shareCommit.zip"

git bundle create "$bundleOut" main
if ($LASTEXITCODE -ne 0) { throw 'bundle 생성 실패' }
git bundle verify "$bundleOut"
if ($LASTEXITCODE -ne 0) { throw 'bundle 검증 실패' }
git archive --format=zip --output="$sourceOut" main
if ($LASTEXITCODE -ne 0) { throw '소스 ZIP 생성 실패' }
Get-FileHash -LiteralPath $bundleOut,$sourceOut -Algorithm SHA256
git rev-parse main
```

출력 해시와 전체 커밋 ID를 배포 목록에 기록하고 파일 동기화가 끝난 뒤 공유합니다. 새 파일명의 main 커밋과 ZIP·bundle의 대상이 같아야 합니다. 커밋하지 않은 수정, 로컬 설정·로그·실행 파일은 위 두 명령에 자동 포함되지 않습니다. 같은 커밋 산출물을 다시 만들 때도 전달 중인 파일을 바꾸지 말고 별도 시각/판본을 붙여 발행합니다.

여러 개발자가 서로의 수정을 돌려받아야 하면 private GitLab/GitHub의 작업 브랜치와 PR/MR 방식으로 전환하는 편이 편합니다. bundle을 계속 써야 한다면 작성자별 작업 브랜치를 별도 bundle로 보내고 유지보수자가 비교·검토한 뒤 통합합니다. 양쪽 PC가 각각 main을 수정해 같은 이름의 bundle로 덮어쓰는 방식은 사용하지 않습니다.

## 개발자 전달 체크리스트

- 공유 bundle·ZIP·API 문서가 같은 커밋 기준인지, SHA256과 커밋 ID가 있는지 확인합니다.
- 개발자는 자기 PC의 독립 clone에서 시작하고 `git status`, `git log`로 기준을 확인합니다.
- [README](../README.md)의 OS별 빌드/검사 프리셋을 사용합니다. 실행 파일은 별도 배포물인지 직접 빌드한 것인지 구분합니다.
- 변경 검토에는 public 헤더·JSON 호환성·추가 메모리와 계산량·실제로 수행한 검사를 포함합니다.
- 서버 토큰·실시간 운용 ini·관측값·개인 장치 설정 백업은 전달 자료에 포함하지 않습니다. 공식 예제 설정은 소스와 함께 전달합니다.
