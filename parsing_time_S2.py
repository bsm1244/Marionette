import os, sys

def read_file_chunks(file_path, chunk_size=2):
    with open(file_path, 'r') as log_file:
        while True:
            lines = log_file.readlines(chunk_size)
            if not lines:
                break
            for line in lines:
                yield line

if __name__ == '__main__':
    chunk_size = 20000000
    file_name = sys.argv[1]

    num_try = 0
    attack_time = 0
    compare_time = 0

    for line in read_file_chunks(file_name, chunk_size):

        if "attack time:" not in line and "compare time:" not in line: continue

        if "attack time:" in line:
            num_try += 1
            a, b, c = map(str, line.strip().split(" "))
            attack_time += int(c)
            # print(attack_time)
        else:
            
            a, b, c = map(str, line.strip().split(" "))
            compare_time += int(c)
            # print(compare_time)

    print(attack_time/num_try, compare_time/num_try)