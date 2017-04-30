#!/bin/bash
touch $1
sum=$((0 + $3))
i=0
while [ $i -lt $sum ]
do
   echo $2 >> $1
   i=$[$i+1]
done
