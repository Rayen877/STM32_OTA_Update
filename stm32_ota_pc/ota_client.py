import json,struct,threading,time
import paho.mqtt.client as mqtt
from config import MQTT_HOST,MQTT_PORT,DEVICE_ID,TOPIC_START,TOPIC_CHUNK,TOPIC_FINISH,TOPIC_COMMAND,TOPIC_STATUS,CHUNK_SIZE,MQTT_QOS
from crc32 import crc32

class OtaClient:
    HEADER_FORMAT="<4sIIHHI"; HEADER_SIZE=20; MAGIC=b"OTA1"
    def __init__(self):
        self.lock=threading.Lock(); self.connected=threading.Event(); self.ready=threading.Event()
        self.image_ready=threading.Event(); self.error=None; self.last_status={}
        self.client=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id=f"pc-ota-{DEVICE_ID}")
        self.client.on_connect=self.on_connect; self.client.on_message=self.on_message
    def on_connect(self,c,u,f,r,p=None):
        if r==0:
            self.connected.set(); c.subscribe(TOPIC_STATUS,qos=MQTT_QOS)
        else: self.error=f"MQTT connection failed: {r}"
    def on_message(self,c,u,msg):
        if msg.topic!=TOPIC_STATUS:return
        try:s=json.loads(msg.payload.decode())
        except:return
        with self.lock:self.last_status=s
        if s.get("state")=="READY":self.ready.set()
        elif s.get("state")=="IMAGE_READY":self.image_ready.set()
        elif s.get("state")=="ERROR":self.error=s.get("message","ESP32 error")
    def connect(self):
        self.client.connect(MQTT_HOST,MQTT_PORT,60); self.client.loop_start()
        if not self.connected.wait(5):raise RuntimeError(self.error or "MQTT connection timeout")
    def disconnect(self):
        try:self.client.loop_stop();self.client.disconnect()
        except:pass
    def status(self):
        with self.lock:return dict(self.last_status)
    def pub_json(self,topic,obj):
        info=self.client.publish(topic,json.dumps(obj,separators=(",",":")),qos=MQTT_QOS);info.wait_for_publish()
        if info.rc!=mqtt.MQTT_ERR_SUCCESS:raise RuntimeError(f"MQTT publish failed: {info.rc}")
    def start(self, version, hw_id, image, build_id, image_crc, metadata_offset=0):
        self.ready.clear();self.image_ready.clear();self.error=None
        self.pub_json(TOPIC_START,{"version":version,"hw_id":hw_id,"size":len(image),"crc32": f"{image_crc:08X}","chunk_size":CHUNK_SIZE,"build_id":build_id,"metadata_offset":metadata_offset})
        if not self.ready.wait(10):raise RuntimeError(self.error or f"No READY: {self.status()}")
    def send_chunks(self, image, progress, image_crc):
        offset=0;seq=0
        while offset<len(image):
            if self.error:raise RuntimeError(self.error)
            payload=image[offset:offset+CHUNK_SIZE]; expected=offset+len(payload)
            frame=struct.pack(self.HEADER_FORMAT,self.MAGIC,seq,offset,len(payload),0,crc32(payload))+payload
            info=self.client.publish(TOPIC_CHUNK,frame,qos=MQTT_QOS);info.wait_for_publish()
            if info.rc!=mqtt.MQTT_ERR_SUCCESS:raise RuntimeError(f"Chunk publish failed: {info.rc}")
            deadline=time.monotonic()+10
            while time.monotonic()<deadline:
                if self.error:raise RuntimeError(self.error)
                s=self.status()
                if s.get("state") == "CHUNK_ACK":
                    seq_ok = ("seq" not in s) or (s.get("seq") == seq)
                    offset_ok = (
                        ("next_offset" not in s and s.get("received") == expected)
                        or s.get("next_offset") == expected
                    )
                    if seq_ok and offset_ok:
                        break
                time.sleep(.01)
            else:raise RuntimeError(f"ACK timeout seq={seq}, status={self.status()}")
            offset=expected;seq+=1
            if progress:progress(offset,len(image))
        self.pub_json(TOPIC_FINISH,{"size":len(image),"crc32": f"{image_crc:08X}"})
        return seq
    def send_command(self, command):
        self.pub_json(TOPIC_COMMAND, {"cmd": command})

    def wait_for_states(self, states, timeout=60):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            s=self.status()
            if s.get("state") in states:
                return s
            time.sleep(0.1)
        raise RuntimeError(self.error or f"Timeout waiting for states {states}: {self.status()}")

    def wait_image_ready(self):
        if self.image_ready.wait(15):return
        raise RuntimeError(self.error or f"No IMAGE_READY: {self.status()}")
