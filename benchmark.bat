@echo off
setlocal EnableExtensions EnableDelayedExpansion

cd /d "%~dp0"

set "OFFICIAL_TEST_DIR=%TEMP%\rinha-backend-2026-official\test"
set "IMAGE=ghcr.io/pastel123so/pastel-de-c:latest"
set "PROJECT=pastel-de-c-bench"
set "CANDIDATE_LIMIT=1000"
set "API_CPUS=0.40"
set "LB_CPUS=0.20"
set "PROFILE_EVERY=0"
set "SKIP_BUILD=0"
set "KEEP_UP=0"

:args
if "%~1"=="" goto after_args
if /I "%~1"=="--skip-build" goto arg_skip_build
if /I "%~1"=="-skip-build" goto arg_skip_build
if /I "%~1"=="--keep-up" goto arg_keep_up
if /I "%~1"=="-keep-up" goto arg_keep_up
if /I "%~1"=="--candidate-limit" goto arg_candidate_limit
if /I "%~1"=="-candidate-limit" goto arg_candidate_limit
if /I "%~1"=="--api-cpus" goto arg_api_cpus
if /I "%~1"=="-api-cpus" goto arg_api_cpus
if /I "%~1"=="--lb-cpus" goto arg_lb_cpus
if /I "%~1"=="-lb-cpus" goto arg_lb_cpus
if /I "%~1"=="--profile-every" goto arg_profile_every
if /I "%~1"=="-profile-every" goto arg_profile_every
echo ERRO: argumento desconhecido: %~1
exit /b 1

:arg_skip_build
set "SKIP_BUILD=1"
shift
goto args

:arg_keep_up
set "KEEP_UP=1"
shift
goto args

:arg_candidate_limit
shift
if "%~1"=="" (
  echo ERRO: faltou valor para candidate-limit
  exit /b 1
)
set "CANDIDATE_LIMIT=%~1"
shift
goto args

:arg_api_cpus
shift
if "%~1"=="" (
  echo ERRO: faltou valor para api-cpus
  exit /b 1
)
set "API_CPUS=%~1"
shift
goto args

:arg_lb_cpus
shift
if "%~1"=="" (
  echo ERRO: faltou valor para lb-cpus
  exit /b 1
)
set "LB_CPUS=%~1"
shift
goto args

:arg_profile_every
shift
if "%~1"=="" (
  echo ERRO: faltou valor para profile-every
  exit /b 1
)
set "PROFILE_EVERY=%~1"
shift
goto args

:after_args
if not exist "docker-compose.yml" (
  echo ERRO: rode este .bat na raiz do repo.
  exit /b 1
)

if not exist "resources\references.bin" (
  echo ERRO: resources\references.bin nao existe.
  echo Copie o arquivo oficial para resources\references.bin.
  exit /b 1
)

if not exist "%OFFICIAL_TEST_DIR%\test.js" (
  echo ERRO: test.js oficial nao existe em:
  echo %OFFICIAL_TEST_DIR%\test.js
  exit /b 1
)

echo bench config:
echo   IMAGE=%IMAGE%
echo   CANDIDATE_LIMIT=%CANDIDATE_LIMIT%
echo   API_CPUS=%API_CPUS%
echo   LB_CPUS=%LB_CPUS%
echo   RINHA_PROFILE_EVERY=%PROFILE_EVERY%
echo.

if "%SKIP_BUILD%"=="0" (
  docker build -t "%IMAGE%" .
  if errorlevel 1 goto fail
)

set "CANDIDATE_LIMIT=%CANDIDATE_LIMIT%"
set "API_CPUS=%API_CPUS%"
set "LB_CPUS=%LB_CPUS%"
set "RINHA_PROFILE_EVERY=%PROFILE_EVERY%"

docker compose -p "%PROJECT%" down --remove-orphans >nul 2>nul
if "%PROFILE_EVERY%"=="0" (
  docker compose -p "%PROJECT%" up -d
) else (
  > "%TEMP%\%PROJECT%-profile-logging.yml" echo services:
  >> "%TEMP%\%PROJECT%-profile-logging.yml" echo   api1:
  >> "%TEMP%\%PROJECT%-profile-logging.yml" echo     logging:
  >> "%TEMP%\%PROJECT%-profile-logging.yml" echo       driver: json-file
  >> "%TEMP%\%PROJECT%-profile-logging.yml" echo   api2:
  >> "%TEMP%\%PROJECT%-profile-logging.yml" echo     logging:
  >> "%TEMP%\%PROJECT%-profile-logging.yml" echo       driver: json-file
  >> "%TEMP%\%PROJECT%-profile-logging.yml" echo   lb:
  >> "%TEMP%\%PROJECT%-profile-logging.yml" echo     logging:
  >> "%TEMP%\%PROJECT%-profile-logging.yml" echo       driver: json-file
  docker compose -p "%PROJECT%" -f docker-compose.yml -f "%TEMP%\%PROJECT%-profile-logging.yml" up -d
)
if errorlevel 1 goto fail

echo esperando /ready...
set "READY=0"
for /L %%i in (1,1,30) do (
  curl.exe -s --max-time 2 http://localhost:9999/ready | findstr /C:"ready" >nul
  if not errorlevel 1 (
    set "READY=1"
    goto ready_done
  )
  timeout /t 1 /nobreak >nul
)

:ready_done
if not "%READY%"=="1" (
  echo ERRO: api nao ficou ready.
  docker compose -p "%PROJECT%" logs --tail=80
  goto fail
)

docker stats --no-stream --format "table {{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}" | findstr "%PROJECT%"

if not exist "%OFFICIAL_TEST_DIR%\test" mkdir "%OFFICIAL_TEST_DIR%\test"
del /f /q "%OFFICIAL_TEST_DIR%\test\results.json" >nul 2>nul

docker run --rm --network host ^
  -e K6_NO_USAGE_REPORT=true ^
  -v "%OFFICIAL_TEST_DIR%:/test" ^
  -w /test ^
  grafana/k6:latest run /test/test.js
if errorlevel 1 goto fail

if exist "%OFFICIAL_TEST_DIR%\test\results.json" (
  type "%OFFICIAL_TEST_DIR%\test\results.json"
) else (
  echo ERRO: results.json nao gerado.
  goto fail
)

if not "%PROFILE_EVERY%"=="0" (
  echo.
  echo === profile logs ===
  docker compose -p "%PROJECT%" -f docker-compose.yml -f "%TEMP%\%PROJECT%-profile-logging.yml" logs --tail=160 api1 api2 lb
)

if "%KEEP_UP%"=="0" docker compose -p "%PROJECT%" down --remove-orphans >nul 2>nul
exit /b 0

:fail
if "%KEEP_UP%"=="0" docker compose -p "%PROJECT%" down --remove-orphans >nul 2>nul
exit /b 1
