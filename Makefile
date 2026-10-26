check:
	lsof /dev/cu.usbmodem11203 /dev/tty.usbmodem11203

free:
	kill -9 $(lsof -t /dev/cu.usbmodem11203 /dev/tty.usbmodem11203)
gui:
	mkdir -p build && cd build && cmake .. && make -j$(nproc) 

start_gui:
	./build/chess_gui
