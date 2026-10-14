param([switch]$Experimental,[switch]$Model,[switch]$TestHost,[switch]$GlobalB1,[switch]$Compact512,[switch]$Compact320,[switch]$Modes,[switch]$BSeries)
$ErrorActionPreference = 'Stop'
$nrRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if($BSeries){$Modes=$true}
if($Compact512 -and $Compact320){throw 'Select one compact geometry'}
if($Modes -and ($Compact512 -or $Compact320)){throw 'Modes build is separate from fixed geometries'}
if($Compact512 -or $Compact320 -or $Modes){$GlobalB1=$true;$Model=$true}
if($Model -or $TestHost){& python (Join-Path $nrRoot 'tools/embed_native_shaders.py') --check;if($LASTEXITCODE -ne 0){throw 'Embedded shader sources are stale'}}
$nrSource = Join-Path $nrRoot 'work/optiscaler-source'
foreach($nrHeader in @('NativeNrControl.h','NativeNrModes.h','NativeNrFrameInput.h','NativeNrFfxState.h','NativeNrFfxBridge.h','NativeNrLifecycle.h','NativeNrHook.h')){
    Copy-Item -LiteralPath (Join-Path $nrRoot "integration/optiscaler/$nrHeader") -Destination (Join-Path $nrSource "OptiScaler/experimental/$nrHeader")
}
if($TestHost -or $GlobalB1){$Model=$true};if($Model){$Experimental=$true}
$nrMode = if($Modes){'global-b1-modes'}elseif($Compact320){'global-b1-compact320'}elseif($Compact512){'global-b1-compact512'}elseif($GlobalB1){'global-b1'}elseif($TestHost){'model-test'}elseif($Model){'model'}elseif ($Experimental) { 'experimental' } else { 'baseline' }
$nrBuild = Join-Path $nrRoot "work/b580/optiscaler-build/$nrMode"
if($BSeries){$nrBuild += '-bseries'}
$nrBin = Join-Path $nrBuild 'bin'
$nrObj = Join-Path $nrBuild 'obj'
$nrVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$nrVs = & $nrVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $nrVs) { throw 'C++ toolchain not found' }
$nrMsbuild = Join-Path $nrVs 'MSBuild/Current/Bin/MSBuild.exe'
foreach ($nrDirectory in @($nrBuild,$nrBin,$nrObj)) { New-Item -ItemType Directory -Path $nrDirectory -Force | Out-Null }
$nrProps = Join-Path $nrBuild 'native_nr_build.props'
$nrDefine = if ($Experimental) { 'OPTISCALER_EXPERIMENTAL_NATIVE_NR;' } else { '' }
if($TestHost){$nrDefine+='OPTISCALER_NATIVE_NR_TEST_HOST;'}
if($Model){$nrDefine+='OPTISCALER_NATIVE_NR_MODEL;'}
if($GlobalB1){$nrDefine+='NR_GAME_GLOBAL_B1;NR_EXPERIMENTAL_LINALG;NR_BRANCH_COMPACT;NR_GLOBAL_NORMALIZATION;NR_C32_COMPACT;NR_ATTENTION_PARTITION;NR_DENSE_TILE;NR_NORM_WAVE;NR_SOFTMAX_WAVE;NR_C32_PROJECTION;NR_WINDOW_SEGMENT;'}
if($Compact512){$nrDefine+='NR_GAME_COMPACT512;'}
if($Compact320){$nrDefine+='NR_GAME_COMPACT320;'}
if($Modes){$nrDefine+='NR_GAME_MODES;'}
$nrModelXml=''
if($Model){
    $nrBaseline=Join-Path $nrRoot 'work/b580/optiscaler-build/baseline/bin/OptiScaler.dll'
    if(-not (Test-Path -LiteralPath $nrBaseline)){throw 'Build baseline first to preserve its export ordinals'}
    $nrToolVersion=Get-ChildItem (Join-Path $nrVs 'VC/Tools/MSVC') -Directory | Sort-Object {[version]$_.Name} -Descending | Select-Object -First 1
    $nrDumpbin=Join-Path $nrToolVersion.FullName 'bin/Hostx64/x64/dumpbin.exe'
    $nrOriginalDef=Get-Content (Join-Path $nrSource 'OptiScaler/Source.def') -Raw
    $nrDeclared=@{};foreach($nrLine in ($nrOriginalDef -split "`n")){if($nrLine.Trim() -match '^([^\s=;]+)'){ $nrDeclared[$Matches[1]]=$true }}
    $nrExtraDef=@();foreach($nrLine in @(& $nrDumpbin /exports $nrBaseline)){
        if($nrLine -match '^\s+(\d+)\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)' -and -not $nrDeclared.ContainsKey($Matches[2])){
            $nrExtraDef+="$($Matches[2]) @$($Matches[1])"
        }
    }
    if($LASTEXITCODE -ne 0){throw 'Cannot inspect baseline export ordinals'}
    $nrApiNames=@('Initialize','Prepare','Evaluate','Reset','Retire','Collect','Stats','Capture','Stop','LastError','PreparationStats','ConfigureQueue')
    for($nrI=0;$nrI -lt $nrApiNames.Count;$nrI++){ $nrExtraDef+="OptiScalerNR_$($nrApiNames[$nrI]) @$([int](2000+$nrI))" }
    $nrDefPath=Join-Path $nrBuild 'native_nr_model.def'
    [IO.File]::WriteAllText($nrDefPath,$nrOriginalDef+"`n"+($nrExtraDef -join "`n")+"`n",[Text.UTF8Encoding]::new($false))
    $nrDefXml=[Security.SecurityElement]::Escape($nrDefPath)
    $nrBackend=[Security.SecurityElement]::Escape((Join-Path $nrRoot 'integration/optiscaler/NativeNrBackend.cpp'))
    $nrNative=[Security.SecurityElement]::Escape((Join-Path $nrRoot 'src/native'))
    $nrIntegration=[Security.SecurityElement]::Escape((Join-Path $nrRoot 'integration/optiscaler'))
    $nrGlobalXml='';$nrSdkInclude=''
    if($GlobalB1){
        $nrRuntime=[Security.SecurityElement]::Escape((Join-Path $nrRoot 'integration/optiscaler/NativeNrRuntime.cpp'))
        $nrSdkInclude=[Security.SecurityElement]::Escape((Join-Path $nrRoot 'work/b580/cooperative-vectors/agility7213/build/native/include'))+';'
        $nrGlobalXml="<ClCompile Include=`"$nrRuntime`"><PrecompiledHeader>NotUsing</PrecompiledHeader></ClCompile>"
    }
    $nrModelXml=@"
  <ItemGroup><ClCompile Include="$nrBackend"><PrecompiledHeader>NotUsing</PrecompiledHeader><FloatingPointModel>Precise</FloatingPointModel></ClCompile>$nrGlobalXml</ItemGroup>
  <ItemDefinitionGroup>
    <ClCompile><AdditionalIncludeDirectories>$nrSdkInclude$nrNative;$nrIntegration;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories></ClCompile>
    <Link><AdditionalDependencies>directml.lib;bcrypt.lib;%(AdditionalDependencies)</AdditionalDependencies><ModuleDefinitionFile>$nrDefXml</ModuleDefinitionFile></Link>
  </ItemDefinitionGroup>
"@
}
$nrXml = @"
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup>
    <ClCompile><PreprocessorDefinitions>${nrDefine}%(PreprocessorDefinitions)</PreprocessorDefinitions></ClCompile>
  </ItemDefinitionGroup>
  $nrModelXml
</Project>
"@
[IO.File]::WriteAllText($nrProps,$nrXml,[Text.UTF8Encoding]::new($false))
# No solution post-build packaging, installer, cleanup, or game directory writes.
# Trailing forward slashes preserve MSBuild directory properties through quoting.
$nrArgs = @(
    (Join-Path $nrSource 'OptiScaler/OptiScaler.vcxproj'), '/t:Build', '/m:2', '/nologo', '/v:minimal',
    '/p:Configuration=Release', '/p:Platform=x64', '/p:PlatformToolset=v145', '/p:CL_MPCount=2',
    '/p:WindowsTargetPlatformVersion=10.0.26100.0', '/p:PostBuildEventUseInBuild=false',
    "/p:SolutionDir=$($nrSource.Replace('\','/'))/", "/p:OutDir=$($nrBin.Replace('\','/'))/",
    "/p:IntDir=$($nrObj.Replace('\','/'))/", "/p:ForceImportBeforeCppTargets=$nrProps",
    "/bl:$(Join-Path $nrBuild 'build.binlog')"
)
& $nrMsbuild @nrArgs 2>&1 | Tee-Object -FilePath (Join-Path $nrBuild 'build.log')
$nrExit = $LASTEXITCODE
if ($nrExit -ne 0) { throw "OptiScaler $nrMode build failed ($nrExit); see $nrBuild/build.log" }
$nrDll = Join-Path $nrBin 'OptiScaler.dll'
if (-not (Test-Path -LiteralPath $nrDll)) { throw 'Build succeeded without expected DLL' }
Get-FileHash -LiteralPath $nrDll -Algorithm SHA256 | Format-List
