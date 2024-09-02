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

    allow = 0
    num_aggrs = 0
    elapsed_time = 0

    for line in read_file_chunks(file_name, chunk_size):

        if "aggressors" not in line and allow == 0: continue

        if "aggressors" in line:
            allow = 1
            a, b, c, d, e, f = map(str, line.strip().split(" "))
            num_aggrs = int(c.split(",")[0])/2
            # print(num_aggrs)
        else:
            allow = 0
            a, b, c = map(str, line.strip().split(" "))
            elapsed_time = int(c)
            print(elapsed_time/num_aggrs)