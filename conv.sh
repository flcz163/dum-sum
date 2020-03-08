ENCODE="UNKNOWN"
for f in `find ./src -name "*"`; do
	ENCODE=`enca $f`
	if [[ $ENCODE == *GB2312 ]]; then
		cp $f tmp.txt -f
		iconv -f gbk -t utf8 tmp.txt > $f
	fi
done
#cp $1 tmp.txt -f
#iconv -f gbk -t utf8 tmp.txt > $1

