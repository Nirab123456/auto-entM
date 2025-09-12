import sounddevice as sd
import soundfile as sf

# Path to your audio file
filename = "D:/microcontroller/auto-entM/python_server/freepik-pure-bliss.mp3"

# Read the file
data, samplerate = sf.read(filename, dtype='float32')

# Play it
print(f"Playing: {filename}")
sd.play(data, samplerate)

# Wait until playback finishes
sd.wait()
print("Done")
