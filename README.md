# Discord-Debaters
Use this bot across 3-5 distinct microVM agents for an active Discord channel in distinct debating profiles. Proof of concept to demonstrate independent memory usage, extreme concurrency, deterministic execution and hardened isolation. Bot is written in C and uses the CJSON, Curl, Lithium libraries.

# Why this exists
1. Traditional Linux containers running LLM agents consume hundreds of megabytes per instance in runtime overhead; our assembly-kernel sandbox shows active microVMs consuming negligible host RAM.
2. Agents evaluate external user input, scrape live data (e.g., via REST APIs), and process potentially untrusted code/prompts. Even if a prompt injection attack compromises one agent's execution stack, hardware-level microVM boundary should guarantees it cannot break context, read sibling memory, or affect the host. 

# How it works
The demonstration below deploys three Discord bots running on microVMs off BareMetal.returninfinity.com. Each microVM is allotted 1vCPU, 16MB RAM. The C code running inside each microVM uses a lightweight POSIX HTTP client stack (libcurl or raw C sockets) and cJSON to communicate asynchronously with Groq and Discord REST endpoints. In this deployment the Discord Bot does not have permission to write to the file system, but can write to memory. I illustrate the sibling separation by writing to the memory via !memstore for one bot, and reading via !memread for the other. See the screengrab below.
<img width="932" height="860" alt="Screenshot 2026-08-25 004801" src="https://github.com/user-attachments/assets/b2ac3d3b-dfee-4855-b743-b510a476e27c" />
You can also view local telemetry using the !sys prompt.  
<img width="747" height="197" alt="image" src="https://github.com/user-attachments/assets/3c1b17e1-bde0-412a-b1a3-897b5c8a6b41" />

Other prompts work as expected. Over time I would like to evolve this into a debating system to further illustrate the microVM benefits, but that will wait for another day. 
<img width="1395" height="262" alt="image" src="https://github.com/user-attachments/assets/e1bb0313-6a19-4d80-93f1-2829c3b20894" />

# Build instructions
For compiling in BareMetal you need a local copy of cjson. Locate the cjson folder and files in the BareMetal-App directory. 
```
git clone https://github.com/davegamble/cjson
```
Update the following in bot_deferred_polling-A.c with the requisite webhook and Groq key. I am using the free OpenAI model through Groq. You can pick an alternate LLM and update your end point accordingly.
```
#define GROQ_API_KEY         "1. INSERT GROQ API KEY HERE"
#define DISCORD_BOT_TOKEN    "2. INSERT ALPHA BOT TOKEN HERE"
#define DISCORD_CHANNEL_ID   "3. INSERT YOUR DISCORD CHANNEL ID HERE"

```
## BareMetal
Get BM_API_KEY from baremetal.returninfinity.com and set as env variable. 
```
git clone https://github.com/ReturnInfinity/BareMetal-App
cp bot_deferred_polling-A.c BareMetal-App/
cd BareMetal-App
./setup.sh
./1-build.sh  bot_deferred_polling-A.c cjson/CJSON.c
./2-run.sh
./3-upload.sh # optional - upload to BareMetal Cloud
```
Repeat with  bot_deferred_polling-B.c and  bot_deferred_polling-C.c to deploy the instances on BareMetal. 

``` ```
This is what you should see upon completion.

``` ```
<img width="912" height="757" alt="image" src="https://github.com/user-attachments/assets/8d2e6ec8-beba-4e7f-baf7-0e2ae067938a" />

## *nix (Linux/BSD/macOS)
 bot_deferred_polling-A.c, etc are also valid standalone C programs - just link against libcurl and libcjson:
```
gcc -g -O3 bot_deferred_polling-A.c -o discord_bot_alpha -lcjson -lcurl -lpthread
gcc -g -O3 bot_deferred_polling-B.c -o discord_bot_beta -lcjson -lcurl -lpthread
gcc -g -O3 bot_deferred_polling-C.c -o discord_bot_gamma -lcjson -lcurl -lpthread
./flaneur
```
