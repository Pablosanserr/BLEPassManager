# BLEPass Manager
Bluetooth Hardware Password Manager project for [nRF5340DK](https://www.nordicsemi.com/Products/Development-hardware/nRF5340-DK). Stores passwords received via Bluetooth or delivers passwords requested by the user.

## How to use
### Build and run application
This application is developed for the [nRF5340DK](https://www.nordicsemi.com/Products/Development-hardware/nRF5340-DK) board.

It must be compiled with nRF Connect SDK version 1.9.1, selecting the *nrf5340dk_nrf5340_cpuapp_ns* board. To build and flash, it is recommended to use (VS Code nRF Connect Extension Pack)[https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-desktop/Download].

It works to communicate with (BLEPass Chrome Extension)[https://github.com/Pablosanserr/BLEPassChromeExtension] or another application that uses the same protocol as BLEPass Chrome Extension.

### Commands
- *list*: displays the list of stored passwords. It does not explicitly display the password, but its URL and username.
- *clear storage*: clears the password vault. This action requires confirmation by the user.
