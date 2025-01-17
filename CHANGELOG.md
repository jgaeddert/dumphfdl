# CHANGELOG

## Version 1.2.0 (2021-11-17)

- Noise floor and signal level estimates are now computed and printed in
  message headers.
- Added --freq-as-squawk option which uses squawk field in the Basestation feed
  to convey HFDL channel frequency on which the position information has been
  received.
- The program can now decode data from I/Q samples piped onto standard input.
  This allows interoperation with I/Q data sources like GNURadio or KiwiSDR.
- Added `--read-buffer-size` option for setting input buffer size when decoding
  from an I/Q file or from standard input.
- Slightly better sensitivity.

## Version 1.1.0 (2021-10-29)

- Improved sensitivity (less CRC errors)
- Reduced CPU usage
- The program now builds and runs under MacOS

## Version 1.0.0 (2021-10-15)

- First public release
