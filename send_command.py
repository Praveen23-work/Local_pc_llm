import serial
import time

SERIAL_PORT = "COM23"   # native USB CDC — this is where Serial.println/read live
BAUD = 115200

KNOWN_KEYS = {
    "flush_ms", "type_ms", "recon_ms",
    "led_ms", "led_r", "led_g", "led_b",
    "ssid", "pass", "host", "key"
}

def prompt_to_command(prompt: str):
    """
    Placeholder for your local LLM. Replace with a real call that
    returns (key, value). Keep it strict: only ever return one of
    the KNOWN_KEYS above, or raise if it can't map confidently.
    """
    p = prompt.lower()
    if "led" in p and ("blink" in p or "speed" in p or "interval" in p):
        if "slow" in p: return ("led_ms", "2000")
        if "fast" in p: return ("led_ms", "200")
    if "led" in p and "color" in p:
        if "red" in p:
            return [("led_r", "64"), ("led_g", "0"), ("led_b", "0")] # Wrapped in brackets
        if "blue" in p:
            return [("led_r", "0"), ("led_g", "0"), ("led_b", "64")] # Wrapped in brackets
        if "green" in p:
            return [("led_r", "0"), ("led_g", "64"), ("led_b", "0")] # Wrapped in brackets

    if "typing" in p and "slow" in p: return ("type_ms", "40")
    if "typing" in p and "fast" in p: return ("type_ms", "5")
    raise ValueError("Could not map that prompt to a known variable")

def send_line(ser, line: str):
    ser.write((line + "\n").encode())
    time.sleep(0.15)
    while ser.in_waiting:
        print("  ESP32:", ser.readline().decode(errors="replace").strip())

def main():
    ser = serial.Serial(SERIAL_PORT, BAUD, timeout=1)
    time.sleep(2)  # let USB CDC settle after opening
    print(f"Connected to {SERIAL_PORT}. Type 'exit' to quit.\n")

    while True:
        prompt = input("Prompt: ").strip()
        if prompt.lower() in ("exit", "quit"):
            break
        try:
            result = prompt_to_command(prompt)
            # allow either a single (key, value) tuple or a list of them
            commands = result if isinstance(result, list) else [result]
            for key, value in commands:
                if key not in KNOWN_KEYS:
                    print(f"  Refused: '{key}' is not a known variable.")
                    continue
                send_line(ser, f"SET {key} {value}")
        except ValueError as e:
            print(f"  {e}")

    ser.close()

if __name__ == "__main__":
    main()