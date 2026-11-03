# Builds Rewired.apk for the Sony a6000 (PlayMemories Camera Apps, Android 2.3.7).
#
#   powershell -ExecutionPolicy Bypass -File build.ps1
#
# The same seven steps as Recipe Lab's build.cmd: ndk-build, aapt, javac, d8, aapt package, zipalign, apksigner
# (v1 signing only: the camera does not understand v2/v3). Toolchain, overridable through the environment:
#   ANDROID_NDK   NDK r16b - the last NDK with the GCC that targets armeabi / Android 2.3
#   ANDROID_SDK   with build-tools 30.0.3 and platforms;android-28
#   JAVA_HOME     JDK 17
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$tools = Join-Path $PSScriptRoot '..\tools'
if (-not $env:ANDROID_NDK) { $env:ANDROID_NDK = Join-Path $tools 'android-ndk-r16b' }
if (-not $env:ANDROID_SDK) { $env:ANDROID_SDK = Join-Path $tools 'android-sdk' }
if (-not $env:JAVA_HOME -or -not (Test-Path (Join-Path $env:JAVA_HOME 'bin\javac.exe'))) {
    # A JRE on PATH has no javac; take a JDK 17 where Windows installers put one.
    $jdk = Get-ChildItem 'C:\Program Files\Java\jdk-17*', 'C:\Program Files\Eclipse Adoptium\jdk-17*' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($jdk) { $env:JAVA_HOME = $jdk.FullName } else { throw 'set JAVA_HOME to a JDK 17' }
}
$BT  = Join-Path $env:ANDROID_SDK 'build-tools\30.0.3'
$AJ  = Join-Path $env:ANDROID_SDK 'platforms\android-28\android.jar'
$JAVA = Join-Path $env:JAVA_HOME 'bin'
foreach ($f in "$BT\aapt.exe", "$BT\zipalign.exe", "$BT\lib\d8.jar", $AJ, "$env:ANDROID_NDK\ndk-build.cmd") {
    if (-not (Test-Path $f)) { throw "missing: $f  (see README: Building)" }
}
function Run($what, [scriptblock]$cmd) {
    Write-Host $what
    & $cmd
    if ($LASTEXITCODE -ne 0) { throw "$what failed" }
}

Run '[0/7] ndk-build (armeabi, android-14 headers, runs on Android 2.3.7)' {
    & "$env:ANDROID_NDK\ndk-build.cmd" "NDK_PROJECT_PATH=$PSScriptRoot" "APP_BUILD_SCRIPT=$PSScriptRoot\jni\Android.mk" `
        "NDK_APPLICATION_MK=$PSScriptRoot\jni\Application.mk" "NDK_LIBS_OUT=$PSScriptRoot\out\libs" "NDK_OUT=$PSScriptRoot\out\obj" -j4
}

foreach ($d in 'out\gen', 'out\classes', 'out\dex', 'out\apklib') { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
New-Item -ItemType Directory -Force out\gen, out\classes, out\dex, out\apklib\lib\armeabi | Out-Null
Copy-Item out\libs\armeabi\librewired.so out\apklib\lib\armeabi\

Run '[1/7] aapt R.java' { & "$BT\aapt.exe" package -f -m -J out\gen -M AndroidManifest.xml -S res -I $AJ }
Run '[2/7] javac' {
    $src = @(Get-ChildItem -Recurse src, out\gen -Filter *.java | ForEach-Object { $_.FullName })
    & "$JAVA\javac.exe" -encoding UTF-8 --release 8 -Xlint:-options -cp $AJ -d out\classes $src
}
Run '[3/7] d8' {
    $classes = @(Get-ChildItem -Recurse out\classes -Filter *.class | ForEach-Object { $_.FullName })
    & "$JAVA\java.exe" -cp "$BT\lib\d8.jar" com.android.tools.r8.D8 --release --min-api 10 --lib $AJ --output out\dex $classes
}
Run '[4/7] aapt package + dex + native lib' { & "$BT\aapt.exe" package -f -M AndroidManifest.xml -S res -I $AJ -F out\unaligned.apk }
Push-Location out\dex;    & "$BT\aapt.exe" add ..\unaligned.apk classes.dex | Out-Null; Pop-Location
Push-Location out\apklib; & "$BT\aapt.exe" add ..\unaligned.apk lib/armeabi/librewired.so | Out-Null; Pop-Location
Run '[5/7] zipalign' { & "$BT\zipalign.exe" -f 4 out\unaligned.apk out\aligned.apk }

Run '[6/7] sign (v1 only; any self-signed key works on the camera)' {
    if (-not (Test-Path debug.keystore)) {
        & "$JAVA\keytool.exe" -genkeypair -keystore debug.keystore -alias rewired -keyalg RSA -keysize 2048 -validity 10000 `
            -storepass android -keypass android -dname 'CN=Rewired'
    }
    $env:PATH = "$JAVA;$env:PATH"   # apksigner.bat runs whatever java is on PATH
    & "$BT\apksigner.bat" sign --ks debug.keystore --ks-pass pass:android --key-pass pass:android --min-sdk-version 10 `
        --v1-signing-enabled true --v2-signing-enabled false --v3-signing-enabled false --out Rewired.apk out\aligned.apk
}
Run '[7/7] verify' { & "$BT\apksigner.bat" verify --min-sdk-version 10 Rewired.apk }
Write-Host "BUILD OK: $PSScriptRoot\Rewired.apk"
