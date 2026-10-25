import serial, time, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
ser = serial.Serial(PORT, 115200, bytesize=8, parity="N",
                    stopbits=1, timeout=1.5)
time.sleep(0.3)
ser.reset_input_buffer()

def send(cmd):
    print(f">>> {cmd}")
    ser.write((cmd + "\n").encode())
    time.sleep(0.2)
    print(f"<<< {ser.readline().decode(errors='replace').strip()}\n")

send("CONNECT")
send("STATE")
send("NEW")
send("STATE")
send("HELLO")
send("DISCONNECT")
send("STATE")

ser.close()
