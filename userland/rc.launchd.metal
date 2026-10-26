# etc/rc with Metal discovery switched on before launchd is born (RC=rc.launchd.metal).
#
# rc.jobs.metalprops and friends put the two discovery properties on IOAcceleratorES from the
# com.redwood.demo job, which races backboardd: a process that has already asked Metal keeps the
# answer it got (an empty device list), so the window server stayed on OpenGL ES even with
# camtl=1. Here process 1 sets them first, so every process launchd starts sees the device.
#
# Needs boot-arg metal=1 (iokit/Kernel/MiniIOMobileFramebuffer.cpp), else the "on" line answers
# 0xe00002c7 and the boot is the ordinary OpenGL ES one. With camtl=1 CoreAnimation composites
# through RedwoodMetal (Mali via AIR->NIR->Panfrost); metal_profile=1 keeps SpringBoard alive (MPS).
/redwood/bin/metal_props on
/redwood/bin/metal_props show
exec /sbin/launchd.ios
