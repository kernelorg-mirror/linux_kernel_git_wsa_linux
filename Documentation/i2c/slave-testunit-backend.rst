================================
Linux I2C slave testunit backend
================================

by Wolfram Sang <wsa@sang-engineering.com> in 2020

This backend can be used to trigger test cases for I2C bus masters which
require a remote device with certain capabilities (and which are usually not so
easy to obtain). Examples include multi-master testing, and SMBus Host Notify
testing. For tests marked with '*', your I2C controller must be able to switch
between master and slave mode because it needs to send data.

Instantiating the device is regular. Example for bus 0, address 0x30:

# echo "slave-testunit 0x1030" > /sys/bus/i2c/devices/i2c-0/new_device

After that, you will have a write-only device listening. Reads will only return
0xff. The device consists of 4 8-bit registers and all must be written to start
a testcase, i.e. you must always write 4 bytes to the device. The registers are:

0x00 CMD   - which test to trigger
0x01 DATAL - configuration byte 1 for the test
0x02 DATAH - configuration byte 2 for the test
0x03 DELAY - delay in n * 100ms until test is started

Commands
--------

0x00 READ_BYTES*
   DATAL - address to read data from
   DATAH - number of bytes to read

This is useful to test if your bus master driver is handling multi-master
correctly. You can trigger the testunit to read bytes from another device on
the bus. If the bus master under test also wants to access the bus, it will be
busy. Example to read 128 bytes from device 0x50 after 500ms of delay:

# i2cset -y 1 0x30 0x00 0x50 0x80 0x05 i

0x01 SMBUS_HOST_NOTIFY*
   DATAL - low byte of the status word to send
   DATAH - high byte of the status word to send

This test will send an SMBUS_HOST_NOTIFY message to the host. Note that the
status word is currently ignored in the Linux Kernel. Example to send a
notification after 100ms:

# i2cset -y 1 0x30 0x01 0x42 0x42 0x01 i
