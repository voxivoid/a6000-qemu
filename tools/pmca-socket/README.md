# pmca-socket: Sony-PMCA-RE backend for the emulator's USB cable

`socket.py` is a USB backend for [Sony-PMCA-RE](https://github.com/ma1co/Sony-PMCA-RE) that talks the "A6KU" protocol
(`docs/reference/RE_USB_HOST_PATH.md` §5) to the emulator's USB device model. Pure Python, no driver, no root, works on macOS.

Apply to a Sony-PMCA-RE checkout (tested on `a82f5ba`):
```sh
cp tools/pmca-socket/socket.py <Sony-PMCA-RE>/pmca/usb/driver/generic/socket.py
cd <Sony-PMCA-RE> && patch -p1 < <emulator>/tools/pmca-socket/sony-pmca-re-socket.patch   # adds -d socket
```

Use:
```sh
./run.sh -M usb-socket=127.0.0.1:7643                   # emulator side
python3 pmca-console.py info -d socket                  # plugs the cable, enumerates, prints the camera info
```
Connecting plugs the virtual cable, the end of the process pulls it. The backend does the host OS's enumeration itself
(bus reset, GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIGURATION) once per attach.

Environment: `A6K_USB=host:port` (default `127.0.0.1:7643`), `A6K_USB_TIMEOUT` transfer timeout in ms (default 30000),
`A6K_USB_ATTACH_WAIT` seconds to wait for the gadget after plugging (default 180), `A6K_USB_REATTACH_WAIT` seconds to wait
for the camera to come back after it dropped the gadget, e.g. for `install` or `updatershell` (default 300),
`A6K_USB_DEBUG=1` prints every message.

License: MIT, like Sony-PMCA-RE (`LICENSE`, Copyright (c) 2015 ma1co). The patch modifies Sony-PMCA-RE files.
