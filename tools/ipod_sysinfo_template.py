#!/usr/bin/env python3
"""Makes firmware/usbstack/ipod_sysinfo_5g.h from the SysInfoExtended an
iPod Video's own firmware gives (ipodpatcher --dump-xml in disk mode): the
XML as a C string, with the iPod's serial number, FireWire GUID and RAM
left as @SERIAL@, @GUID@ and @RAM@ for usb_storage.c to fill in.

    tools/ipod_sysinfo_template.py SysInfoExtended firmware/usbstack/ipod_sysinfo_5g.h
"""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
xml = open(src, encoding='utf-8').read()

for key, token in (('SerialNumber', '@SERIAL@'), ('FireWireGUID', '@GUID@')):
    xml, n = re.subn(r'(<key>%s</key>\s*<string>)[^<]*(</string>)' % key,
                     r'\g<1>%s\g<2>' % token, xml)
    assert n == 1, key
xml, n = re.subn(r'(<key>RAM</key>\s*<integer>)\d+(</integer>)',
                 r'\g<1>@RAM@\g<2>', xml)
assert n == 1, 'RAM'

out = ['/* Made by tools/ipod_sysinfo_template.py from the SysInfoExtended of',
       '   an iPod Video 5.5G with Apple\'s firmware 1.3, as its disk mode gave',
       '   it; @SERIAL@, @GUID@ and @RAM@ are the iPod\'s own. */',
       'static const char ipod_xml_template[] =']
for line in xml.splitlines(keepends=True):
    esc = line.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
    out.append('"%s"' % esc)
out[-1] += ';'
open(dst, 'w', encoding='utf-8').write('\n'.join(out) + '\n')
