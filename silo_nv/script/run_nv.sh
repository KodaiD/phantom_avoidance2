output_file="nv_result.csv"

rratio=10
range_size=10000

skew=0
num_tuples=1000000
num_execution=10

echo $output_file

echo 1
numactl --interleave=all ./silo.exe -clocks_per_us=2100 -extime=3 -max_ope=1 -rmw=0 -tuple_num=$num_tuples -ycsb=1 -rratio=$rratio -zipf_skew=$skew -range_size=$range_size -thread_num=1 -num_execution=$num_execution >> $output_file
for ((thread=28; thread<=224; thread+=28))
do
  echo $thread
  numactl --interleave=all ./silo.exe -clocks_per_us=2100 -extime=3 -max_ope=1 -rmw=0 -tuple_num=$num_tuples -ycsb=1 -rratio=$rratio -zipf_skew=$skew -range_size=$range_size -thread_num=$thread -num_execution=$num_execution >> $output_file
done
