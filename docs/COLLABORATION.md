# 다른 PC에서 가져오기와 개발자 협업

공용 소스 저장소는 [VIPSfef/HyRUS_EnvData_Router](https://github.com/VIPSfef/HyRUS_EnvData_Router)이며 기본 브랜치는 `main`입니다. 이 저장소에서는 호스트 MAVLink 수신·분석·서버 전송만 개발합니다. I²C 드라이버·PX4 uORB·MAVLink 송신·펌웨어는 `HyRUS_EnvData_Px4`에서 개발하고 [입력 규약](PX4_MAVLINK_CONTRACT.md)으로 연결합니다. 기존 펌웨어 소스는 별도 로컬 작업 폴더로 옮겼으며, 그 저장소의 원격 게시·보드 업로드는 별도 작업입니다.

실행 프로그램을 전달할 때는 [배포 폴더 안내](../release/README.md)의 `release/v0.11.0/assets/`를 사용하세요. 아래 OneDrive bundle 절차는 소스와 Git 이력을 오프라인으로 공유하는 별도 방법입니다.

다른 Windows x64 PC에는 공유용 ZIP 하나만 복사하면 됩니다. 압축을 모두 풀고 `Start-Sync.cmd`를 실행하면 처음에는 소스와 문서, Git 변경 이력을 가져옵니다. 나중에 새 ZIP을 받아 같은 방법으로 실행하면 기존 작업 폴더를 업데이트합니다. 필요한 Git도 ZIP에 들어 있으므로 받는 PC에서 별도 설치나 다운로드를 할 필요가 없습니다.

## ZIP 하나로 처음 가져오기와 업데이트하기

공유용 ZIP의 이름은 `mavlink-router-handoff-커밋번호.zip`이며, 발행 PC의 `build\sharing` 폴더에 있습니다. 실행 파일 배포물이 아니라 소스·문서를 Git으로 가져오는 데 필요한 자료를 한데 모은 파일입니다.

공유용 ZIP은 **Windows x64용**입니다. Windows에 기본으로 있는 PowerShell 5.1과 ZIP 안의 Git으로 실행합니다. Git이나 Python을 따로 설치하지 않아도 되며, 업데이트할 때 인터넷의 Git 서버에 접속할 필요도 없습니다.

Git은 공식 Git for Windows의 MinGit 배포본을 `runtime\git`에 넣었습니다. 배포본에 포함된 라이선스 문서도 함께 보관합니다. 스크립트는 이 Git을 직접 사용하며, 다른 프로그램을 자동 설치하거나 PC의 PATH를 바꾸지 않습니다. **ZIP 전체를 풀고 `runtime` 폴더를 스크립트 옆에 그대로 두세요.** 스크립트만 복사해서 실행하면 필요한 Git을 찾지 못할 수 있습니다.

1. 전달받은 공유용 ZIP을 `다운로드\mavlink-router-share`처럼 **자료를 전달받을 폴더**에 풉니다. 기존 소스 작업 폴더에 풀지 마세요.
2. 압축을 푼 폴더 안에 `Start-Sync.cmd`, `Sync-MavlinkRouter.ps1`, `LATEST.json`, `.bundle` 파일과 `runtime` 폴더가 함께 있는지 확인합니다.
3. **`Start-Sync.cmd`를 두 번 클릭합니다.** 처음 가져오기인지 업데이트인지 스크립트가 알아서 판단합니다. 완료되거나 오류가 나면 창에 결과가 표시되고, 확인할 때까지 창이 유지됩니다.

PowerShell에서 직접 실행하고 싶다면 압축을 푼 폴더에서 아래 명령을 사용해도 됩니다.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Sync-MavlinkRouter.ps1
```

압축을 풀었을 때의 `source` 폴더에는 소스와 문서가 모두 들어 있습니다. 스크립트를 실행하기 전에도 `source\docs\SERVER_API.md`를 열어 서버 API를 읽을 수 있습니다. 문서끼리의 링크도 그대로 사용할 수 있습니다.

처음 실행하면 기본적으로 **`C:\Users\사용자이름\dev\mavlink_telemetry_data_router`**에 Git 작업 폴더를 만듭니다. 정확한 기본값은 `%USERPROFILE%\dev\mavlink_telemetry_data_router`입니다. 완료되면 그 폴더의 `README.md`와 `docs\SERVER_API.md`를 열어 보세요. 소스를 빌드하거나 수집 프로그램을 실행하는 작업은 별도로 해야 합니다.

명령의 `ExecutionPolicy Bypass`는 이번에 실행한 PowerShell 프로세스에만 적용됩니다. PC의 실행 정책 설정을 영구적으로 바꾸지 않습니다.

**다음 버전을 받을 때도 같은 방법으로 실행합니다.** 새 공유용 ZIP을 받아 새 전달 폴더에 풀고 그 안의 `Start-Sync.cmd`를 두 번 클릭하세요. ZIP을 푼 폴더가 달라도 기본 작업 폴더는 같으므로, 스크립트가 기존 Git 작업 폴더를 찾아 새 커밋만 적용합니다. PowerShell을 사용한다면 새 전달 폴더에서 앞의 명령을 실행합니다. ZIP은 만들 당시의 사본이므로, 예전 ZIP의 스크립트를 반복 실행하는 것만으로 아직 전달받지 않은 새 버전까지 가져오지는 못합니다.

작업 폴더를 바꾸고 싶다면 처음부터 `-Destination`으로 지정하세요. 이후 업데이트할 때도 같은 경로를 사용합니다.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Sync-MavlinkRouter.ps1 -Destination 'D:\dev\mavlink_telemetry_data_router'
```

### OneDrive 공유 폴더를 계속 읽게 하려면

발행자가 OneDrive의 공유 폴더를 계속 갱신한다면 ZIP을 매번 받을 필요 없이 그 폴더를 지정할 수도 있습니다. OneDrive 동기화와 파일 내려받기가 끝난 뒤 실행하세요. 다음 경로는 이 프로젝트의 예시이므로 이 PC의 실제 공유 폴더로 바꿉니다.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Sync-MavlinkRouter.ps1 -SharedDirectory 'C:\Users\사용자이름\OneDrive\mavlink-router-share'
```

`-SharedDirectory`를 생략하면 스크립트가 들어 있는 폴더를 읽습니다. 그곳의 `LATEST.json`에서 최신 bundle과 해시를 확인하고, bundle을 `%LOCALAPPDATA%\mavlink-router\bundles`로 복사한 뒤 Git으로 처리합니다. 로컬 사본의 위치도 바꾸려면 `-CacheDirectory 'D:\cache\mavlink-router'`를 추가하세요. 이 옵션은 소스 작업 폴더의 위치를 바꾸지 않습니다.

전달 폴더와 소스 작업 폴더는 서로 다른 폴더로 두세요. 소스를 수정하는 작업 폴더는 OneDrive 밖에 두는 것을 권장합니다. 스크립트의 원본은 [support/sharing/Sync-MavlinkRouter.ps1](../support/sharing/Sync-MavlinkRouter.ps1)입니다.

### 업데이트가 멈추면

스크립트는 수정하거나 새로 만든 파일이 남아 있거나, 현재 브랜치가 `main`이 아니거나, 로컬에 별도 커밋이 있어 받은 버전으로 곧바로 이어질 수 없으면 멈춥니다. Git 저장소가 아닌 기존 폴더, 다른 프로젝트의 이력, 파일 해시 불일치도 오류로 알려 줍니다.

이때 화면에 나온 이유를 확인하세요. 수정한 작업은 개발자와 함께 커밋하거나 별도로 보관한 뒤 다시 실행하면 됩니다. 스크립트가 작업을 자동으로 지우거나 숨기거나, 강제로 덮어쓰지는 않습니다. 다른 프로젝트가 들어 있는 폴더라면 `-Destination`으로 새 경로를 지정하세요.

## 직접 명령을 실행하는 방법 — 고급 참고

자동 스크립트를 사용한다면 이 절은 건너뛰어도 됩니다. Git이 어떤 순서로 동작하는지 확인하거나 수동으로 처리하고 싶은 개발자를 위한 참고입니다. 아래 예시는 `git` 명령을 사용할 수 있는 개발 PC를 기준으로 합니다. 공유용 ZIP을 실행하는 PC에는 Git을 따로 설치할 필요가 없습니다. Windows 명령은 **PowerShell** 기준이며, 오류가 나면 다음 단계로 넘어가지 말고 내용을 확인합니다.

### OneDrive로 받은 bundle 준비하기

OneDrive는 bundle을 전달하는 용도로 사용합니다. 실제 작업 폴더는 `C:\dev\mavlink_telemetry_data_router`처럼 OneDrive 밖에 두세요. 같은 `.git` 폴더를 여러 PC가 동시에 동기화하며 수정하면 각 PC의 작업과 Git 내부 파일이 충돌할 수 있습니다.

공유 폴더에는 다음 파일이 있습니다.

| 파일 | 용도 |
|---|---|
| `Start-Sync.cmd` | 두 번 클릭해서 자동 가져오기·업데이트를 시작합니다. |
| `Sync-MavlinkRouter.ps1` | 처음 가져오기와 이후 업데이트를 한 번에 처리하는 스크립트입니다. |
| `runtime\git\` | 스크립트가 사용하는 Windows x64 Git과 배포본의 라이선스 문서입니다. 공유용 ZIP 안에 들어 있습니다. |
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
$sharedDir = Join-Path $env:OneDrive 'mavlink-router-share'
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
shared_dir='/PATH/TO/OneDrive/mavlink-router-share'
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

## GitHub에서 개발자와 함께 수정하기

bundle은 소스와 변경 이력을 오프라인으로 전달하는 방법입니다. 온라인 협업은 지정된 GitHub 저장소를 각 개발자의 PC에 복제하고 작업 브랜치에서 수정한 뒤 PR로 검토합니다. 저장소 공개 여부와 협업 권한은 GitHub에서 소유자가 관리합니다.

기본 브랜치는 `main`입니다. 아래 명령의 작업 폴더는 본인의 경로로 바꾸세요. 저장소 URL은 `https://github.com/VIPSfef/HyRUS_EnvData_Router.git`입니다.

### 소유자가 처음 한 번 할 일

기존 로컬 이력을 연결할 때는 원격의 초기 README·라이선스와 충돌하는지 먼저 확인합니다. 토큰을 URL에 넣지 말고 Git 자격증명 관리자나 SSH 인증을 사용하세요. 아래 `remote add`는 origin이 없는 경우에만 실행합니다.

```powershell
Set-Location 'C:\dev\mavlink_telemetry_data_router'
git status --short
git remote -v
# origin이 아직 없을 때 실행합니다.
git remote add origin 'https://github.com/VIPSfef/HyRUS_EnvData_Router.git'
git push -u origin main
```

이미 `origin`이 있으면 `git remote -v`로 지정한 저장소인지 확인하세요. 원격 이력을 덮어쓰는 강제 push는 사용하지 않습니다.

### GitHub에 소스와 실행 파일을 구분해 올리기

소스 변경을 검토하고 커밋한 뒤 `main`을 원격 저장소에 올립니다. 운영용 `telemetry.ini`, 실제 토큰, 수집 자료와 빌드 폴더는 커밋하지 않습니다. 생성한 `release/v0.11.0/`도 소스 이력에 넣는 대신 GitHub Release의 첨부 파일로 전달합니다. 소스 ZIP은 해당 배포 시점의 소스 사본이며 `.git` 이력을 포함하지 않습니다.

GitHub Release 본문은 [0.11.0 릴리스 노트](releases/v0.11.0.md)를 사용하고, `release/v0.11.0/assets/`의 Windows ZIP·Linux TAR.GZ·소스 ZIP·`SHA256SUMS.txt`·`manifest.json`을 첨부합니다. 태그와 게시 대상 커밋을 확정한 뒤 게시하세요. 빌드 당시 미커밋 수정이 있었다면 manifest의 소스 상태를 확인하고, 태그용 커밋을 만든 후 동일한 소스로 다시 묶어야 합니다.

프로젝트 자체의 라이선스는 소유자가 결정해야 합니다. 배포본의 제3자 라이선스 고지는 프로젝트 전체에 대한 라이선스 선택을 대신하지 않습니다.

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
git clone 'https://github.com/VIPSfef/HyRUS_EnvData_Router.git' 'C:\dev\mavlink_telemetry_data_router'
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

이 절은 새 배포본을 만드는 사람만 읽으면 됩니다. 발행 PC에는 Git과 Python 3.9 이상이 필요하지만, ZIP을 받는 PC에는 따로 설치할 프로그램이 없습니다. 먼저 소스와 문서를 검토하고 `main`에 커밋하세요.

공식 Git for Windows에서 Windows x64 MinGit ZIP을 준비하고, 발행할 때 사용할 SHA256을 확인합니다. 아래 두 인수는 **발행 PC에 준비한 실제 파일 경로와 확인한 64자리 SHA256**으로 바꿔 실행하세요.

```powershell
python support/sharing/package_handoff.py `
  --git-runtime-archive 'C:\tools\MinGit-WINDOWS-X64.zip' `
  --git-runtime-sha256 '확인한_SHA256_64자리'
```

발행 도구는 지정한 MinGit ZIP의 해시를 검사한 뒤, 파일과 라이선스 문서를 `runtime\git`에 함께 넣습니다. 받는 사람에게는 **`build\sharing`의 `mavlink-router-handoff-커밋번호.zip` 하나**만 전달하세요. 그 안에 실행 스크립트, Git, 전체 이력이 담긴 bundle, 배포 목록, 안내와 검증용 해시가 들어 있습니다. `source` 아래에는 소스와 문서 전체가 펼쳐져 있어 Git 작업을 하기 전에도 읽을 수 있습니다.

발행 도구가 별도로 만드는 bundle과 소스 ZIP은 개발자용 산출물입니다. 받는 사람에게 따로 복사하도록 요구하지 않습니다.

### bundle과 소스 ZIP만 직접 만드는 방법 — 고급 참고

아래 명령은 Git을 이미 사용하는 개발자가 bundle과 소스 ZIP만 만들 때 참고하세요. MinGit을 포함한 공유용 ZIP은 만들지 않으므로, **받는 사람에게 파일 하나만 전달하려면 위의 발행 도구를 사용합니다.**

받는 PC의 명령이 최신 파일을 찾으려면 발행자가 `LATEST.json`도 함께 갱신해야 합니다. 소스 변경을 검토하고 `main`에 커밋한 다음, 아래 순서로 묶으세요. 전체 이력을 포함하므로 처음 받는 PC도 같은 bundle을 사용할 수 있습니다.

다음은 배포 파일을 만드는 예시입니다. 기존 파일을 덮어쓰지 않고, 같은 커밋의 배포본이 이미 있으면 멈춥니다. 새 bundle과 ZIP이 완성된 뒤 마지막으로 `LATEST.json`을 바꿉니다. 원격 저장소로 게시하는 명령은 포함하지 않습니다.

```powershell
$ErrorActionPreference = 'Stop'
Set-Location 'C:\dev\mavlink_telemetry_data_router'
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
| [PX4 → Router 입력 규약](PX4_MAVLINK_CONTRACT.md) | 별도 `HyRUS_EnvData_Px4`의 송신 구현과 합의할 메시지·단위·시각·소스·결측 처리 |

공유하는 bundle과 ZIP에는 커밋된 소스와 문서가 들어갑니다. bundle에는 Git 이력도 포함됩니다. 실행 파일이 필요하다면 `release/v0.11.0/assets/`의 해당 OS용 배포물을 별도로 전달하세요. 로컬 운용 설정, 인증 토큰과 관측 자료는 공유 대상에 넣지 않습니다. 과거에 커밋한 내용은 Git 이력에 남으므로, 처음 외부 개발자에게 전달할 때는 이력까지 포함해 공유 범위를 확인하세요.
