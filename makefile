include config.mk

#-C是指定目录
#make -C signal   
#可执行文件应该放最后
#make -C app      
#用shell命令for搞，shell里边的变量用两个$
all:
	@for dir in $(BUILD_DIR); \
	do \
		make -C $$dir; \
	done

clean:
	rm -rf app/link_obj app/dep nginx
	rm -rf signal/*.gch app/*.gchi
