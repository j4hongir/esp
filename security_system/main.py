import network, urequests, ujson, ntptime, utime
from machine import Pin, ADC, PWM
import config

IDLE, ARMED, ALARM, COOLDOWN = "IDLE", "ARMED", "ALARM", "COOLDOWN"

state        = IDLE
ldr_base     = 0
alarm_t      = 0
cool_t       = 0
sensor_t     = 0
tg_t         = 0
update_id    = 0
btn_prev     = 1
btn_t        = 0
beep_on      = False
beep_t       = 0
event_log    = []
TG = "https://api.telegram.org/bot" + config.BOT_TOKEN


def log(msg):
    ts = time_str()
    entry = ts + " " + msg
    print(entry)
    event_log.append(entry)
    if len(event_log) > 15:
        event_log.pop(0)


def time_str():
    try:
        t = utime.localtime(utime.time() + config.UTC_OFFSET)
        return "{:02d}.{:02d} {:02d}:{:02d}:{:02d}".format(t[2], t[1], t[3], t[4], t[5])
    except:
        return "N/A"


pir    = Pin(config.PIR_PIN, Pin.IN)
ldr    = ADC(Pin(config.LDR_PIN))
ldr.atten(ADC.ATTN_11DB)
ldr.width(ADC.WIDTH_12BIT)
buzzer = PWM(Pin(config.BUZZER_PIN), freq=1000, duty=0)
led    = Pin(config.LED_PIN, Pin.OUT)
led.value(0)
btn    = Pin(config.BTN_PIN, Pin.IN, Pin.PULL_UP)
log("[HW] OK")


def wifi_connect():
    w = network.WLAN(network.STA_IF)
    w.active(True)
    if not w.isconnected():
        w.connect(config.WIFI_SSID, config.WIFI_PASS)
        for _ in range(15):
            if w.isconnected():
                break
            utime.sleep(1)
    if w.isconnected():
        log("[WiFi] " + w.ifconfig()[0])
    else:
        log("[WiFi] FAIL")

wifi_connect()

try:
    ntptime.settime()
    log("[NTP] OK")
except Exception as e:
    log("[NTP] FAIL: " + str(e))


def tg_send(text):
    try:
        r = urequests.post(
            TG + "/sendMessage",
            headers={"Content-Type": "application/json"},
            data=ujson.dumps({"chat_id": config.CHAT_ID, "text": text})
        )
        r.close()
    except Exception as e:
        print("[TG] send err: " + str(e))


def tg_poll():
    global update_id, tg_t
    now = utime.ticks_ms()
    if utime.ticks_diff(now, tg_t) < config.TG_POLL_MS:
        return
    tg_t = now
    try:
        r = urequests.post(
            TG + "/getUpdates",
            headers={"Content-Type": "application/json"},
            data=ujson.dumps({"offset": update_id + 1, "timeout": 1, "limit": 5})
        )
        data = r.json()
        r.close()
        if data.get("ok"):
            for u in data.get("result", []):
                update_id = u.get("update_id", update_id)
                txt = u.get("message", {}).get("text", "").strip()
                if txt:
                    handle(txt)
    except Exception as e:
        print("[TG] poll err: " + str(e))


def handle(cmd):
    global state, ldr_base

    if cmd == "/start":
        tg_send(
            "/arm     - activate\n"
            "/disarm  - deactivate\n"
            "/status  - current state\n"
            "/time    - current time\n"
            "/log     - event log\n"
            "/test    - test LED & buzzer"
        )

    elif cmd == "/arm":
        if state == IDLE:
            ldr_base = ldr.read()
            set_state(ARMED)
            tg_send("ARMED | LDR base=" + str(ldr_base) + " | " + time_str())
        else:
            tg_send("Already: " + state)

    elif cmd == "/disarm":
        if state != IDLE:
            hw_off()
            set_state(IDLE)
            tg_send("DISARMED | " + time_str())
        else:
            tg_send("Already IDLE")

    elif cmd == "/status":
        tg_send(
            "State: " + state + "\n"
            "PIR: " + ("MOTION" if pir.value() else "clear") + "\n"
            "LDR: " + str(ldr.read()) + " (base=" + str(ldr_base) + ")\n"
            "BTN: " + str(btn.value()) + "\n"
            + time_str()
        )

    elif cmd == "/time":
        tg_send(time_str())

    elif cmd == "/log":
        if event_log:
            tg_send("\n".join(event_log[-10:]))
        else:
            tg_send("Log empty")

    elif cmd == "/test":
        tg_send("Testing LED + buzzer for 3s...")
        led.value(1)
        buzzer.duty(512)
        utime.sleep_ms(500)
        buzzer.freq(2000)
        utime.sleep_ms(500)
        buzzer.freq(1000)
        utime.sleep_ms(500)
        buzzer.duty(0)
        buzzer.freq(1000)
        led.value(0)
        tg_send("Test done")

    else:
        tg_send("Unknown: " + cmd + "\nTry /start")


def set_state(s):
    global state
    log("[FSM] " + state + " -> " + s)
    state = s


def hw_off():
    buzzer.duty(0)
    led.value(0)


def beep_toggle():
    global beep_on, beep_t
    now = utime.ticks_ms()
    if utime.ticks_diff(now, beep_t) >= config.BEEP_MS:
        beep_on = not beep_on
        buzzer.duty(512 if beep_on else 0)
        led.value(1 if beep_on else 0)
        beep_t = now


def check_btn():
    global state, btn_prev, btn_t
    cur = btn.value()
    if btn_prev == 1 and cur == 0:
        btn_t = utime.ticks_ms()
    if cur == 0 and btn_t > 0:
        if utime.ticks_diff(utime.ticks_ms(), btn_t) >= config.DEBOUNCE_MS:
            if state != IDLE:
                hw_off()
                set_state(IDLE)
                btn_t = 0
                log("[BTN] manual reset")
    btn_prev = cur


def update_fsm():
    global state, ldr_base, alarm_t, cool_t, sensor_t
    now = utime.ticks_ms()
    if utime.ticks_diff(now, sensor_t) < config.SENSOR_MS:
        return
    sensor_t = now

    if state == IDLE:
        return

    elif state == ARMED:
        pir_hit = pir.value() == 1
        ldr_val = ldr.read()
        ldr_hit = abs(ldr_val - ldr_base) > config.LDR_DELTA
        if pir_hit or ldr_hit:
            reason = ("PIR" if pir_hit else "") + (" LDR delta=" + str(abs(ldr_val - ldr_base)) if ldr_hit else "")
            alarm_t = now
            set_state(ALARM)
            log("[ALARM] " + reason)
            tg_send("!! ALARM !! " + reason + " | " + time_str())

    elif state == ALARM:
        beep_toggle()
        if utime.ticks_diff(now, alarm_t) >= config.ALARM_MS:
            hw_off()
            cool_t = now
            set_state(COOLDOWN)
            tg_send("Alarm off, cooldown " + str(config.COOLDOWN_MS // 1000) + "s | " + time_str())

    elif state == COOLDOWN:
        if utime.ticks_diff(now, cool_t) >= config.COOLDOWN_MS:
            ldr_base = ldr.read()
            set_state(ARMED)
            tg_send("ARMED again | " + time_str())


log("[BOOT]")
tg_send("BOOT" + time_str() + "\nsend /start")

while True:
    check_btn()
    update_fsm()
    tg_poll()
    utime.sleep_ms(10)
