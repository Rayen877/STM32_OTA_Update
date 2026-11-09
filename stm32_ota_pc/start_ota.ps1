$mosquittoExe = "C:\Program Files\Mosquitto\mosquitto.exe"
$mosquittoConf = "C:\Program Files\Mosquitto\mosquitto.conf"
$dashboardDir = "C:\stm32_ota_pc"
$pythonExe = "$dashboardDir\.venv\Scripts\python.exe"

# Start Mosquitto with the config file
Start-Process powershell.exe -ArgumentList `
    "-NoExit", `
    "-Command", `
    "& '$mosquittoExe' -c '$mosquittoConf' -v"

# Start Dashboard using virtual environment
Start-Process powershell.exe -ArgumentList `
    "-NoExit", `
    "-Command", `
    "cd '$dashboardDir'; & '$pythonExe' -m uvicorn app:app --host 127.0.0.1 --port 8000"

# Open dashboard
Start-Sleep -Seconds 2
Start-Process "http://127.0.0.1:8000"