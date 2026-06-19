import serial
import time

port = "COM6"
baud = 115200

print(f"Opening {port} at {baud} baud...")
try:
    # Opening the port usually resets the ESP32 via DTR/RTS
    ser = serial.Serial(port, baud, timeout=1)
    
    # Toggle DTR/RTS to ensure reset
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setDTR(True)
    ser.setRTS(False)
    
    with open("boot_logs.txt", "w", encoding="utf-8", errors="ignore") as f:
        end_time = time.time() + 15  # read for 15 seconds
        while time.time() < end_time:
            line = ser.readline()
            if line:
                try:
                    text = line.decode('utf-8', errors='ignore')
                    f.write(text)
                    print(text, end='')
                except Exception as e:
                    print(f"Error decoding line: {e}")
    ser.close()
    print("Finished reading logs.")
except Exception as e:
    print(f"Serial port error: {e}")
