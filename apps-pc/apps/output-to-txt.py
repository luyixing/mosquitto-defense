
File = open("cmds.txt", "w")
for x in range(1,1000):
	clientid = "app_"+str(x)
	strr = "[\"python3\", \""+clientid+".py\"],"
	File.write("    "+strr+"\n")

File.close()