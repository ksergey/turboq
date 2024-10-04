import pyturboq

consumer = pyturboq.create_consumer('turboq.spsc')

while True:
    str = consumer.dequeue()
    if len(str) > 0:
        print(str)
