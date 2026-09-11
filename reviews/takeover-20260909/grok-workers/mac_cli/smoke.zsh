export PATH="$HOME/.local/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"
cursor-agent --help > help.txt 2>&1
echo "help_exit=$?" > smoke.txt
cursor-agent status >> smoke.txt 2>&1
echo "status_exit=$?" >> smoke.txt
cursor-agent --version >> smoke.txt 2>&1
cursor-agent models > models.txt 2>&1 || cursor-agent --list-models > models.txt 2>&1
echo "$?" > exit.txt
