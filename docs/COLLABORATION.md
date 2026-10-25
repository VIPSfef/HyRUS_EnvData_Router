# 다른 PC에서 가져오기와 개발자 협업

OneDrive로 받은 Git bundle을 사용하면 인터넷의 Git 서버에 접속하지 않고도 소스와 문서, 변경 이력을 가져올 수 있습니다. 처음에는 `git clone`으로 작업 폴더를 만들고, 이후에는 새 bundle을 받아 `git fetch`와 `git merge`로 갱신합니다.

아래 Windows 명령은 **PowerShell**에서 실행하세요. 각 단계에서 오류가 나면 다음 단계로 넘어가지 말고 오류 내용을 확인하세요. Git이 설치되어 있어야 하며, `git --version`으로 확인할 수 있습니다.

## OneDrive로 받은 bundle 사용하기

OneDrive는 bundle을 전달하는 용도로 사용합니다. 실제 작업 폴더는 `C:\dev\mavlink_telemetry_data_router`처럼 OneDrive 밖에 두세요. 같은 `.git` 폴더를 여러 PC가 동시에 동기화하며 수정하면 각 PC의 작업과 Git 내부 파일이 충돌할 수 있습니다.

공유 폴더에는 다음 파일이 있습니다.

| 파일 | 용도 |
|---|---|
| `LATEST.json` | 가장 최근에 공유한 bundle의 이름, 커밋 번호, SHA256을 알려 줍니다. |
| `mavlink-router-커밋번호.bundle` | 소스·문서와 `main`의 전체 변경 이력을 담습니다. Git으로 가져올 때 사용합니다. |
| `mavlink-router-source-커밋번호.zip` | 해당 커밋의 소스와 문서를 읽기 편하게 묶은 파일입니다. Git 이력은 들어 있지 않습니다. |
| `manifest-커밋번호.json`, `SHA256SUMS-커밋번호.txt` | 특정 배포본의 파일 목록과 해시를 보관합니다. |
| `README-FIRST.txt` | 전달받은 사람이 먼저 읽을 안내입니다. |

### 1. 최신 bundle을 이 PC에 복사하기 — 매번 실행

처음 가져올 때도, 나중에 업데이트할 때도 이 단계부터 시작합니다. 먼저 OneDrive에서 공유 폴더의 동기화가 끝났는지 확인하세요. 온라인 전용 파일이라면 내려받기를 마친 뒤 진행합니다.

다른 PC의 OneDrive 폴더 위치가 다르면 **`$sharedDir` 설정 줄만 실제 공유 폴더로 바꾸세요.** `$env:OneDrive`가 비어 있거나 회사 계정의 다른 폴더를 사용한다면 폴더의 전체 경로를 직접 넣으면 됩니다.

```powershell
$ErrorActionPreference = 'Stop'
$sharedDir = Join-Path $env:OneDrive '문서\New project 3\mavlink-server-transfer\build\sharing'
$release = Get-Content -LiteralPath (Join-Path $sharedDir 'LATEST.json') -Raw | ConvertFrom-Json
$localDir = Join-Path $env:LOCALAPPDATA 'mavlink-router\bundles'
New-Item -ItemType Directory -Force -Path $localDir | Out-Null
$bundleLocal = Join-Path $localDir $release.bundle.file
Copy-Item -LiteralPath (Join-Path $sharedDir $release.bundle.file) -Destination $bundleLocal -Force

$actualHash = (Get-FileHash -LiteralPath $bundleLocal -Algorithm SHA256).Hash
if ($actualHash -ne $release.bundle.sha256) {
    throw '파일이 배포 목록과 다릅니다. OneDrive 동기화가 끝난 뒤 다시 복사하세요.'
}
$repoDir = 'C:\dev\mavlink_telemetry_data_router'
```

이 명령은 `LATEST.json`에서 파일 이름을 읽으므로 커밋 번호를 직접 고칠 필요가 없습니다. 해시가 일치하면 로컬 사본을 사용할 준비가 된 것입니다. 공유 폴더의 파일을 직접 읽으며 Git 작업을 하지 않고, 이 PC의 별도 사본을 사용합니다.

### 2. 처음 가져오는 PC에서만 실행

1번을 실행한 **같은 PowerShell 창**에서 이어서 실행하세요. `$repoDir`은 아직 사용하지 않는 새 폴더여야 합니다. 기존 작업 폴더가 있다면 삭제하지 말고 3번 업데이트 절차를 사용하세요.

```powershell
New-Item -ItemType Directory -Force -Path (Split-Path $repoDir -Parent) | Out-Null
git clone --origin onedrive --branch main "$bundleLocal" "$repoDir"
if ($LASTEXITCODE -ne 0) { throw '가져오지 못했습니다. bundle과 작업 폴더 경로를 확인하세요.' }

git -C "$repoDir" bundle verify "$bundleLocal"
if ($LASTEXITCODE -ne 0) { throw 'bundle을 검증하지 못했습니다.' }
$receivedCommit = (git -C "$repoDir" rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $receivedCommit -ne $release.source_commit) {
    throw '가져온 커밋이 배포 목록과 다릅니다.'
}
git -C "$repoDir" log -5 --oneline
```

이제 `$repoDir` 아래의 `README.md`와 `docs/SERVER_API.md`를 열면 됩니다. `onedrive`는 가져온 bundle을 가리키는 Git 원격 이름입니다. 별도의 서버가 실행되는 것은 아니며, 이 bundle에 변경 내용을 push할 수는 없습니다. [Git 공식 bundle 설명](https://git-scm.com/docs/git-bundle)

### 3. 나중에 새 버전으로 업데이트하기

새 bundle이 공유되면 **1번을 다시 실행**하세요. 그러면 `$bundleLocal`이 새 파일을 가리킵니다. 같은 PowerShell 창에서 다음을 실행합니다.

```powershell
git -C "$repoDir" status --short
if ($LASTEXITCODE -ne 0) { throw '작업 폴더를 확인하세요.' }
$localChanges = git -C "$repoDir" status --porcelain
if ($LASTEXITCODE -ne 0) { throw '로컬 변경 상태를 확인하지 못했습니다.' }
if ($localChanges) { throw '수정한 파일이 있습니다. 먼저 작업을 커밋하거나 보관하세요.' }

git -C "$repoDir" bundle verify "$bundleLocal"
if ($LASTEXITCODE -ne 0) { throw 'bundle을 검증하지 못했습니다.' }
git -C "$repoDir" remote set-url onedrive "$bundleLocal"
if ($LASTEXITCODE -ne 0) { throw 'onedrive 연결을 확인하세요. 처음 복제할 때 지정한 이름이어야 합니다.' }
git -C "$repoDir" fetch onedrive
if ($LASTEXITCODE -ne 0) { throw '새 이력을 가져오지 못했습니다.' }
$fetchedCommit = (git -C "$repoDir" rev-parse onedrive/main).Trim()
if ($LASTEXITCODE -ne 0 -or $fetchedCommit -ne $release.source_commit) {
    throw '새 bundle의 커밋이 배포 목록과 다릅니다.'
}

# 아직 작업 파일에 적용하기 전입니다. 먼저 바뀔 커밋을 확인합니다.
git -C "$repoDir" log --oneline --left-right main...onedrive/main
if ($LASTEXITCODE -ne 0) { throw '변경 이력을 비교하지 못했습니다.' }
git -C "$repoDir" switch main
if ($LASTEXITCODE -ne 0) { throw 'main 브랜치로 이동하지 못했습니다.' }
git -C "$repoDir" merge --ff-only onedrive/main
if ($LASTEXITCODE -ne 0) { throw '이력이 갈라져 있습니다. 강제로 덮어쓰지 말고 변경 내용을 검토하세요.' }
$updatedCommit = (git -C "$repoDir" rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $updatedCommit -ne $release.source_commit) {
    throw '로컬 main에 추가 커밋이 있습니다. 받은 버전과 로컬 이력을 비교하세요.'
}
git -C "$repoDir" log -5 --oneline
```

`fetch`는 새 이력을 가져오고, `merge --ff-only`는 기존 이력에서 그대로 이어지는 경우에만 작업 파일을 갱신합니다. 로컬 `main`에 별도 커밋을 만들었다면 자동 적용을 거부할 수 있습니다. 이때 로컬 작업을 지우는 `reset`이나 `clean`으로 해결하지 말고, 어느 변경을 합칠지 검토하세요.

커밋하지 않은 작업을 잠시 보관하려면 원래 작업 브랜치에서 아래 명령을 선택적으로 사용할 수 있습니다. 업데이트를 마친 뒤에는 원래 브랜치로 돌아와 복원하세요. 복원 중 충돌이 나면 내용을 확인해야 합니다.

```powershell
git stash push --include-untracked -m 'before-shared-update'
git stash list
# 업데이트를 마치고 원래 작업 브랜치로 돌아온 뒤 실행합니다.
git stash apply 'stash@{0}'
```

`stash apply`는 보관한 사본을 자동 삭제하지 않습니다. Git이 무시하는 빌드 파일과 운용 설정은 이 명령에 포함되지 않습니다.

### Linux에서도 같은 bundle을 사용할 수 있습니다

아래 예시의 공유 폴더 경로는 실제 위치로 바꾸세요. 첫 명령 묶음은 최신 bundle을 로컬에 복사하고 해시를 검사합니다. `python3`는 `LATEST.json`을 읽는 데 사용합니다.

```sh
shared_dir='/PATH/TO/OneDrive/문서/New project 3/mavlink-server-transfer/build/sharing'
local_dir="$HOME/.cache/mavlink-router/bundles"
repo_dir="$HOME/dev/mavlink_telemetry_data_router"
mkdir -p "$local_dir" "$HOME/dev"
bundle_name=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["bundle"]["file"])' "$shared_dir/LATEST.json")
expected_hash=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["bundle"]["sha256"])' "$shared_dir/LATEST.json")
bundle_local="$local_dir/$bundle_name"
cp "$shared_dir/$bundle_name" "$bundle_local" && printf '%s  %s\n' "$expected_hash" "$bundle_local" | sha256sum -c -
```

마지막 검사에 성공하면 처음에는 아래 명령을 사용합니다. 명령이 실패하면 다음 단계로 넘어가지 마세요.

```sh
git clone --origin onedrive --branch main "$bundle_local" "$repo_dir"
git -C "$repo_dir" bundle verify "$bundle_local"
git -C "$repo_dir" log -5 --oneline
```

나중에 업데이트할 때는 새 bundle을 복사하고 해시를 검사한 뒤 다음을 실행합니다. `git status --short`에 수정한 파일이 나오면 먼저 작업을 보존하세요.

```sh
git -C "$repo_dir" status --short
git -C "$repo_dir" bundle verify "$bundle_local" &&
git -C "$repo_dir" remote set-url onedrive "$bundle_local" &&
git -C "$repo_dir" fetch onedrive &&
git -C "$repo_dir" log --oneline --left-right main...onedrive/main &&
git -C "$repo_dir" switch main &&
git -C "$repo_dir" merge --ff-only onedrive/main
```

## 개발자와 함께 수정할 때: 비공개 GitLab 또는 GitHub

bundle은 소스와 변경 이력을 전달하기에 편리합니다. 여러 사람이 수정을 주고받는 단계에서는 회사 GitLab이나 GitHub의 **비공개 저장소**를 공용 원본으로 사용하는 편이 좋습니다. 각 개발자가 자기 PC에 복제하고 작업 브랜치에서 수정한 뒤, PR 또는 MR로 검토를 요청하는 방식입니다.

현재 원본 소스는 `C:\Users\pjs20\OneDrive\문서\New project 3\mavlink-server-transfer`에 있으며 기본 브랜치는 `main`입니다. 이 문서를 작성할 때 원격 저장소는 아직 연결하지 않았습니다. `prj.uaslaboratory.synology.me`의 Git 경로는 실제 서비스에서 발급한 주소를 확인해야 합니다.

### 소유자가 처음 한 번 할 일

비공개 빈 저장소를 만들고 개발자를 초대하세요. 기존 이력을 올릴 것이므로 웹 화면에서 README나 라이선스를 새로 만들지 않습니다. 아래 주소는 **예시**입니다. 실제 저장소 URL로 바꾸고, 토큰을 URL에 넣지 말고 Git 자격증명 관리자나 SSH 인증을 사용하세요. [GitHub 저장소 만들기](https://docs.github.com/en/repositories/creating-and-managing-repositories/creating-a-new-repository)

```powershell
Set-Location 'C:\Users\pjs20\OneDrive\문서\New project 3\mavlink-server-transfer'
git status --short
git remote -v
# origin이 아직 없을 때 실행합니다.
git remote add origin 'https://gitlab.example.com/TEAM/mavlink_telemetry_data_router.git'
git push -u origin main
```

GitHub를 사용한다면 `https://github.com/YOUR_ORG/mavlink_telemetry_data_router.git`와 같은 실제 저장소 주소를 사용합니다. 이미 `origin`이 있으면 기존 주소부터 확인하세요. 이 문서의 명령은 게시 방법을 설명한 것이며, 작성 과정에서 원격 게시를 실행하지는 않았습니다.

읽기와 코드 수정에 필요한 권한은 다음처럼 구분할 수 있습니다.

| 참여 목적 | GitLab의 예 | GitHub 조직 저장소의 예 |
|---|---|---|
| 소스 확인과 검토 | Reporter | Read |
| 개발 및 작업 브랜치 올리기 | Developer | Write |
| 병합 및 저장소 유지보수 | Maintainer | Maintain 또는 관리자가 지정한 권한 |

개인 GitHub 저장소는 조직 저장소와 권한 체계가 다릅니다. 개인 저장소에 협업자로 초대한 사람을 읽기 전용 사용자라고 생각하면 안 됩니다. 세부 권한을 나누려면 GitHub 조직이나 회사 GitLab을 사용하세요. [GitLab 역할](https://docs.gitlab.com/user/permissions/), [GitHub 조직 역할](https://docs.github.com/en/organizations/managing-user-access-to-your-organizations-repositories/managing-repository-roles/repository-roles-for-an-organization), [개인 저장소 권한](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/repository-access-and-collaboration/permission-levels-for-a-personal-account-repository)

### 개발자가 처음 가져올 때

인터넷의 Git 저장소에서 처음 복제한다면 다음과 같이 실행합니다. 이미 bundle에서 복제한 작업 폴더는 그대로 두고 `git remote add origin '실제 URL'`로 서버 연결을 추가할 수도 있습니다.

```powershell
New-Item -ItemType Directory -Force -Path 'C:\dev' | Out-Null
git clone 'https://gitlab.example.com/TEAM/mavlink_telemetry_data_router.git' 'C:\dev\mavlink_telemetry_data_router'
Set-Location 'C:\dev\mavlink_telemetry_data_router'
git log -5 --oneline
```

개발 중에는 작업 브랜치를 만드세요. 다음 예시의 브랜치와 파일 이름은 실제 작업에 맞게 바꿉니다.

```powershell
git switch -c feature/describe-the-change
# 코드와 문서를 수정하고 필요한 검사를 실행합니다.
git add -- src/changed_file.c docs/changed_document.md
git commit -m 'Describe the change'
git push -u origin feature/describe-the-change
```

그런 다음 GitHub에서는 PR, GitLab에서는 MR을 만들어 검토를 요청합니다. 변경 이유와 바뀐 동작, 검사 결과, API 호환성에 미치는 영향을 적으면 검토자가 이해하기 쉽습니다. [GitHub PR](https://docs.github.com/en/pull-requests/reference/pull-requests), [GitLab MR](https://docs.gitlab.com/user/project/merge_requests/)

## 새 bundle을 발행하는 사람의 작업

받는 PC의 명령이 최신 파일을 찾으려면 발행자가 `LATEST.json`도 함께 갱신해야 합니다. 소스 변경을 검토하고 `main`에 커밋한 다음, 아래 순서로 묶으세요. 전체 이력을 포함하므로 처음 받는 PC도 같은 bundle을 사용할 수 있습니다.

다음은 배포 파일을 만드는 예시입니다. 기존 파일을 덮어쓰지 않고, 같은 커밋의 배포본이 이미 있으면 멈춥니다. 새 bundle과 ZIP이 완성된 뒤 마지막으로 `LATEST.json`을 바꿉니다. 원격 저장소로 게시하는 명령은 포함하지 않습니다.

```powershell
$ErrorActionPreference = 'Stop'
Set-Location 'C:\Users\pjs20\OneDrive\문서\New project 3\mavlink-server-transfer'
if (git status --porcelain) { throw '먼저 변경 내용을 검토하고 커밋하세요.' }
$commit = (git rev-parse main).Trim()
if ($LASTEXITCODE -ne 0) { throw 'main 커밋을 확인하지 못했습니다.' }
$short = $commit.Substring(0, 12)
$shareDir = Join-Path (Get-Location).Path 'build\sharing'
New-Item -ItemType Directory -Force -Path $shareDir | Out-Null
$bundleOut = Join-Path $shareDir "mavlink-router-$short.bundle"
$sourceOut = Join-Path $shareDir "mavlink-router-source-$short.zip"
if ((Test-Path -LiteralPath $bundleOut) -or (Test-Path -LiteralPath $sourceOut)) {
    throw '이 커밋의 배포 파일이 이미 있습니다. 전달 중인 파일은 덮어쓰지 마세요.'
}

git bundle create "$bundleOut" main
if ($LASTEXITCODE -ne 0) { throw 'bundle을 만들지 못했습니다.' }
git bundle verify "$bundleOut"
if ($LASTEXITCODE -ne 0) { throw 'bundle을 검증하지 못했습니다.' }
$bundleHead = git bundle list-heads "$bundleOut" refs/heads/main
if ($LASTEXITCODE -ne 0 -or ($bundleHead -split ' ')[0] -ne $commit) {
    throw '묶는 도중 main이 변경되었습니다. 배포 파일과 커밋을 다시 확인하세요.'
}
git -c core.autocrlf=false -c core.eol=lf archive --format=zip --output="$sourceOut" "$commit"
if ($LASTEXITCODE -ne 0) { throw '소스 ZIP을 만들지 못했습니다.' }

$release = [ordered]@{
    format = 'mavlink-router-handoff/v1'
    source_commit = $commit
    source_branch = 'main'
    bundle = @{ file = (Split-Path $bundleOut -Leaf); sha256 = (Get-FileHash -LiteralPath $bundleOut -Algorithm SHA256).Hash.ToLowerInvariant() }
    source_zip = @{ file = (Split-Path $sourceOut -Leaf); sha256 = (Get-FileHash -LiteralPath $sourceOut -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$json = $release | ConvertTo-Json -Depth 4
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $shareDir "manifest-$short.json"), $json, $utf8)
$tempLatest = Join-Path $shareDir ('LATEST.' + [guid]::NewGuid().ToString('N') + '.tmp')
[IO.File]::WriteAllText($tempLatest, $json, $utf8)
Move-Item -LiteralPath $tempLatest -Destination (Join-Path $shareDir 'LATEST.json') -Force
```

파일별 SHA256과 커밋 번호가 같은 배포 목록에 기록됩니다. OneDrive는 파일마다 동기화가 끝나는 시점이 다를 수 있으므로, 받는 사람은 해시를 검사한 뒤 사용해야 합니다. 파일을 복사하는 도중 해시가 다르게 나오면 동기화가 끝난 뒤 1번부터 다시 실행하세요.

## 개발자에게 전달할 자료

| 읽을 자료 | 알 수 있는 내용 |
|---|---|
| [README](../README.md) | 프로그램 실행 방법, 설정과 빌드 방법 |
| [서버 API](SERVER_API.md) | 수신 서버가 구현할 처리, 데이터 항목, 중복과 재전송 처리 |
| [OpenAPI·JSON Schema·예제](api/README.md) | 기계로 검사할 수 있는 규격과 실제 관측값이 없는 요청 예제 |
| [기술 참조](REFERENCE.md) | 각 설정과 시각·좌표·출력의 자세한 규칙 |
| [검증 기록](REVIEW.md) | 실행한 검사와 아직 확인하지 못한 범위 |
| `src/`, `CMakeLists.txt`, `CMakePresets.json` | 구현과 Windows/Linux x64 빌드 구성 |
| `src/tests/`와 CI 설정 | 회귀 검사와 자동 빌드 구성 |
| [PX4 추가 모듈](../support/px4/) | 센서 드라이버와 MAVLink 송신 변경, 적용 기준 커밋 |

공유하는 bundle과 ZIP에는 커밋된 소스와 문서가 들어갑니다. bundle에는 Git 이력도 포함됩니다. 실행 파일이 필요하다면 `build/packages/`의 해당 OS용 배포물을 별도로 전달하세요. 로컬 운용 설정, 인증 토큰과 관측 자료는 공유 대상에 넣지 않습니다. 과거에 커밋한 내용은 Git 이력에 남으므로, 처음 외부 개발자에게 전달할 때는 이력까지 포함해 공유 범위를 확인하세요.
