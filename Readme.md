# MecanumRobot

This is a small repo for the remote controlled mecanum robot. It uses an ESP32 for the web front end part, an Arduino Leonardo for the actual stepper motor control with help of a CNC shield. 

Esp32 exposes the remote control with a webpage, which connects back to Esp32 using websockets. 

Platform.io framework is used in this repo, and using a pre build scripts, a powershell scrip compresses the html pages, converts them to byte arrays, and saves them into a header file as a const array. This is is very space efficient and equivalent to saving the files to SPIFFS.   