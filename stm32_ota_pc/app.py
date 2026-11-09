from pathlib import Path
import shutil,threading,uuid,struct
from fastapi import FastAPI,File,Form,UploadFile
from fastapi.responses import HTMLResponse,JSONResponse
from config import APP_BASE_ADDRESS,CHUNK_SIZE, MQTT_HOST, MQTT_PORT, TOPIC_COMMAND, TOPIC_STATUS, MQTT_QOS, DEVICE_ID
from crc32 import crc32, crc32_with_zero_range
from elf_metadata import read_metadata_from_elf,patch_bin_metadata
from ota_client import OtaClient

BASE=Path(__file__).parent; WORK=BASE/"work"; WORK.mkdir(exist_ok=True)
app=FastAPI(title="STM32 OTA Dashboard"); lock=threading.Lock()
state={"state":"IDLE","progress":0,"message":"Waiting for firmware","metadata":None}

def setstate(**kw):
    with lock:state.update(kw)
def getstate():
    with lock:return dict(state)

def worker(binpath,elfpath,version,build,hw,job):
    c=OtaClient()
    try:
        setstate(state="PARSING",progress=0,message="Reading ELF metadata")
        meta=read_metadata_from_elf(elfpath,APP_BASE_ADDRESS); image=binpath.read_bytes()
        image = patch_bin_metadata(
            image,
            meta,
            version or None,
            build or None
        )

        actual_v = version or f"{meta.version_major}.{meta.version_minor}.{meta.version_patch}"
        actual_b = build or meta.build_id or "pc-upload"
        actual_hw = hw or f"0x{meta.hardware_id:08X}"

        # image_crc32 is stored at metadata offset + 24
        image_crc = struct.unpack_from(
            "<I",
            image,
            meta.bin_offset + 24
        )[0]
        setstate(state="CONNECTING",progress=0,message="Connecting to MQTT",
                 metadata={"version":actual_v,"hw_id":actual_hw,"size":len(image),
                           "crc32": f"{image_crc:08X}","chunk_size":CHUNK_SIZE,"build_id":actual_b})
        c.connect();setstate(state="STARTING",progress=0,message="Sending START")
        c.start(
        actual_v,
        actual_hw,
        image,
        actual_b,
        image_crc,
        meta.bin_offset
        )
        setstate(state="READY",progress=0,message="ESP32 accepted firmware")
        def prog(done,total):setstate(state="TRANSFERRING",progress=int(done*100/total),message=f"Sending chunks: {done}/{total} bytes")
        n = c.send_chunks(image, prog, image_crc)
        setstate(state="VERIFYING",progress=100,message=f"Sent {n} chunks; waiting for ESP32 verification")
        c.wait_image_ready();setstate(state="IMAGE_READY",progress=100,message="Firmware stored and CRC verified by ESP32")
    except Exception as e:setstate(state="ERROR",message=str(e))
    finally:c.disconnect()



def flash_worker():
    c=OtaClient()
    try:
        setstate(state="STM32_FLASH_REQUESTED",progress=100,message="Waiting for STM32 bootloader; press RESET on Nucleo if needed")
        c.connect()
        c.send_command("flash")
        setstate(state="STM32_FLASHING",progress=0,message="ESP32 is sending firmware to STM32")
        s=c.wait_for_states({"STM32_UPDATE_COMPLETE","ERROR"},timeout=120)
        if s.get("state")=="ERROR":
            raise RuntimeError(s.get("detail") or s.get("message") or "ESP32 reported an STM32 update error")
        setstate(state="STM32_UPDATE_COMPLETE",progress=100,message="STM32 firmware updated successfully")
    except Exception as e:
        setstate(state="ERROR",message=str(e))
    finally:
        c.disconnect()

@app.get("/",response_class=HTMLResponse)
def index():return (BASE/"templates/index.html").read_text()

@app.get("/api/status")
def status():return JSONResponse(getstate())

@app.post("/api/upload")
async def upload(bin_file:UploadFile=File(...),elf_file:UploadFile=File(...),version:str=Form(""),build_id:str=Form(""),hw_id:str=Form("")):
    job=uuid.uuid4().hex[:12]; bp=WORK/f"{job}_{Path(bin_file.filename or 'firmware.bin').name}"; ep=WORK/f"{job}_{Path(elf_file.filename or 'firmware.elf').name}"
    with bp.open("wb") as f:shutil.copyfileobj(bin_file.file,f)
    with ep.open("wb") as f:shutil.copyfileobj(elf_file.file,f)
    setstate(state="QUEUED",progress=0,message="Firmware uploaded; starting OTA")
    threading.Thread(target=worker,args=(bp,ep,version,build_id,hw_id,job),daemon=True).start()
    return {"ok":True,"job_id":job}


@app.post("/api/flash")
def flash():
    current=getstate()
    if current.get("state") not in {"IMAGE_READY","STM32_UPDATE_COMPLETE"}:
        return JSONResponse({"ok":False,"message":"Firmware is not ready in ESP32 staging"},status_code=409)
    setstate(state="STM32_FLASH_REQUESTED",progress=100,message="Preparing STM32 update")
    threading.Thread(target=flash_worker,daemon=True).start()
    return {"ok":True}
