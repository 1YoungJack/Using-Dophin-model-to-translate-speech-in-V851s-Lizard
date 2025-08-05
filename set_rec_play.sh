#!/bin/bash


amixer -D hw:audiocodec cset name='LINEOUT Output Select' 1
amixer -D hw:audiocodec cset name='LINEOUT Switch' 1
amixer -D hw:audiocodec cset name='LINEOUT volume' 15


amixer -D hw:audiocodec cset name='MIC1 Input Select' 0
amixer -D hw:audiocodec cset name='MIC1 Switch' 1
amixer -D hw:audiocodec cset name='MIC1 gain volume' 30


#arecord -D hw:audiocodec -f S16_LE -t wav -c1 -r 16000 -d 3 t.wav
#aplay t.wav
#
#arecord -D hw:audiocodec -f S16_LE -t wav -c1 -r 44100 -d 3 t.wav
