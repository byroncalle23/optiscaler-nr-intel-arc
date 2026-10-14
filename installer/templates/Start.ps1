param([switch]$VerifyOnly)
$ErrorActionPreference='Stop'
# Resolve modules from this PowerShell installation, not an inherited pwsh path.
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility') -Force
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Management') -Force
$nrIdentity=[Security.Principal.WindowsIdentity]::GetCurrent()
if(-not $VerifyOnly -and -not ([Security.Principal.WindowsPrincipal]::new($nrIdentity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Ejecuta el lanzador como administrador.'}
$nrGame=Split-Path $PSScriptRoot -Parent
$nrProfile=Get-Content (Join-Path $PSScriptRoot 'profile.json') -Raw | ConvertFrom-Json
if([IO.Path]::GetFileName($nrProfile.executable) -ne $nrProfile.executable -or [IO.Path]::GetExtension($nrProfile.executable) -ne '.exe'){throw 'Nombre de ejecutable no valido.'}
if($nrProfile.dll -notin @('dxgi.dll','version.dll','winmm.dll')){throw 'Nombre de DLL no valido.'}
if(-not $VerifyOnly -and (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($nrProfile.executable)) -ErrorAction SilentlyContinue)){throw 'Cierra el juego elegido antes de iniciar.'}
if((Get-FileHash -LiteralPath (Join-Path $nrGame $nrProfile.executable)).Hash -ne $nrProfile.exe_sha256){throw 'La version del juego cambio.'}
if((Get-FileHash -LiteralPath (Join-Path $nrGame $nrProfile.dll)).Hash -ne $nrProfile.dll_sha256){throw 'La DLL NR no coincide.'}
foreach($nrItem in @($nrProfile.runtime_files)+@($nrProfile.asset_manifests)){
 $nrFile=[IO.Path]::GetFullPath((Join-Path $nrGame $nrItem.path))
 if(-not $nrFile.StartsWith(([IO.Path]::GetFullPath($nrGame)+[IO.Path]::DirectorySeparatorChar),[StringComparison]::OrdinalIgnoreCase)){throw 'Ruta de paquete no valida.'}
 if((Get-FileHash -LiteralPath $nrFile).Hash -ne $nrItem.sha256){throw 'Un archivo del paquete cambio.'}
}
if($VerifyOnly){Write-Output 'Perfil, DLL, SDK y manifiestos verificados. No se abrio el juego ni se modifico Windows.';exit 0}
$nrMutex=[Threading.Mutex]::new($false,'Global\CodexNrB580LinAlgDeveloperMode');$nrOwned=$false
try {
 try{$nrOwned=$nrMutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$nrOwned=$true}
 if(-not $nrOwned){throw 'Hay otra prueba experimental activa.'}
 $nrKey='HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock';$nrProperty='AllowDevelopmentWithoutDevLicense'
 $nrExisted=Test-Path -LiteralPath $nrKey
 $nrOriginal=Get-ItemProperty -LiteralPath $nrKey -ErrorAction SilentlyContinue
 $nrHad=$null -ne $nrOriginal -and $null -ne $nrOriginal.PSObject.Properties[$nrProperty]
 $nrValue=if($nrHad){$nrOriginal.$nrProperty}else{$null}
 if(-not (Test-Path -LiteralPath $nrKey)){New-Item -Path $nrKey -Force | Out-Null}
 New-ItemProperty -LiteralPath $nrKey -Name $nrProperty -Value 1 -PropertyType DWord -Force | Out-Null
 try {
  Start-Process -FilePath (Join-Path $nrGame $nrProfile.executable) -WorkingDirectory $nrGame
  $nrSeen=$false;$nrQuiet=$null;$nrDeadline=[DateTime]::UtcNow.AddSeconds(120)
  do {
   if(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($nrProfile.executable)) -ErrorAction SilentlyContinue){$nrSeen=$true;$nrQuiet=$null}
   elseif($nrSeen -and $null -eq $nrQuiet){$nrQuiet=[DateTime]::UtcNow}
   if(-not $nrSeen -and [DateTime]::UtcNow -gt $nrDeadline){throw 'No aparecio el proceso del juego.'}
   if($null -ne $nrQuiet -and ([DateTime]::UtcNow-$nrQuiet).TotalSeconds -ge 10){break}
   Start-Sleep -Seconds 1
  }while($true)
 }finally{if($nrHad){Set-ItemProperty -LiteralPath $nrKey -Name $nrProperty -Value $nrValue}else{Remove-ItemProperty -LiteralPath $nrKey -Name $nrProperty};if(-not $nrExisted){Remove-Item -LiteralPath $nrKey};Write-Output 'Modo de desarrollador restaurado.'}
}finally{if($nrOwned){$nrMutex.ReleaseMutex()};$nrMutex.Dispose()}
