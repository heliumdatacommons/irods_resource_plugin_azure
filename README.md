# irods_resource_plugin_azure
To build the Azure Resource Plugin, you will need to have:

 - the iRODS Development Tools (irods-dev[el] and irods-runtime) installed for your platform
     http://irods.org/download

And two Microsoft specific libraries. We have forks of these libraries on the helium data commons website

 - cpprest: You can get the library code with the command 

	- git clone https://github.com/heliumdatacommons/cpprestsdk.git

The azure storage lib requires version 2.9.1 of cpprest.  In order to get the required version build a version of the library compatible with the azure resource plugin, you need to switch to the 2.9.1 branch of this library.  To do this the commands are:

	- git checkout 2.9.1-helium

 - azurestorage: you can get the code for this library with the command
	
	-  git clone https://github.com/heliumdatacommons/azure-storage-cpp.git

You'll also need the irods resource plugin azure wrapper,  This is a piece of code that isolates the
azure storage lib from all of the other code it calls, allowing us to use g++ versions of all the
system libraries, with irods and it's development tools, which are built using clang

   - git clone https://github.com/heliumdatacommons/irods_resource_plugin_azure_wrapper.git

Plus a set of more or less standard linux libraries that seem to be required by cpprest and azurestorarage

 - libssl-dev[el]/libssl-devel

 - openssl-dev[el]/openssl

 - glib2-dev[el]

 - sigc++

 - libsigc++20-devel

 - glibmm24-dev[el]

 - libxml++-dev[el]

 - libxml++-dev[el]

 - and all the boost libs.  A few words about boost: it turns out that (as of February 2018) the
   latest versions of boost and cmake are incompatible.  Boost versions 1.6.1 and cmake version 3.10.2
   are compatible. Or you can use the cmake in the irods-externals package

Once you have the prereqs installed you can make the azure resource plugin as follows:

# Build lib cpprest
# Get the code:
git clone https://github.com/heliumdatacommons/cpprestsdk.git

# Switch to the required branch
git checkout 2.9.1-helium

cd /yourrootdir/ccprestsdk/Release

mkdir build.release

cd build.release

# Run cmake
/opt/irods-externals/cmake3.5.2-0/bin/cmake .. -DBOOST_ROOT:STRING=/path/to/boost (often /usr/local)

# Build libccprest
make cpprest

# install puts files in /usr/local/[lib,include]
sudo make install

# Build the azure storage library
git clone https://github.com/heliumdatacommons/azure-storage-cpp.git

cd /yourrootdir/azure-strorage-cpp/Microsoft.WindowsAzure.Storage/

mkdir build.release

cd build.release

# Run cmake:  Set CASABLANCA_DIR to where cpprest is installed (mostly /usr/local)
CASABLANCA_DIR=/yourccprestdir(often /usr/local)  /opt/irods-externals/cmake3.5.2-0/bin/cmake .. 

# Make the library: ignore the compile warnings
make azurestorage

# install puts files in /usr/local/[lib,include]
sudo make install

# Build the azure storage plugin wrapper
git clone https://github.com/heliumdatacommons/irods_resource_plugin_azure_wrapper.git

cd irods_resource_plugin_azure_wrapper

mkdir build.release

cd build.release

/opt/irods-externals/cmake3.5.2-0/bin/cmake .. -DAZURE_EXTERNALS_LIBDIR:STRING=/location/of/azureandcpprestlibs (often /usr/local/lib) -DBOOST_ROOT:STRING=/location-of-boost-directory

# Make the library
make azurestorage

# install puts files in /usr/local/[lib,include]
sudo make install

# Build the azure storage plugin
git clone https://github.com/heliumdatacommons/irods_resource_plugin_azure.git

cd irods_resource_plugin_azure

mkdir build.release

cd build.release

/opt/irods-externals/cmake3.5.2-0/bin/cmake .. -DCRYPTO_LIBDIR:STRING=/usr/lib64 -DAZURE_WRAPPER_DIR:STRING=/location-of-wrapper-lib (often /usr/local)

make package

# Install the package depending on your package manager
sudo dpkg -i irods-resource-plugin-azure_2.2.0~xenial_amd64.deb

sudo rpm -i --force irods-resource-plugin-azure-2.2.0-1.x86_64.rpm

# Configuring the resource
The azure driver is configured as a normal irods compound resource. The context string must have a variable called AZURE_ACCOUNT_FILE.  This variable specifies the location (readable by the irods server) of the azure account file. This file must have exactly 2 lines: the first line is a valid azure account and the second line is the valid key for the account.

# Special issues

# Host name
The azure infrastructure will create host names that are longer than the 64 character limit in the iRODS software. This has a couple of implications. First, the irods installation will freeze and the resulting irods configuration file breaks the icommands. Second, the test code for the azure driver will fail. Third, any client installation won't work.  The following work arounds seem helpful for now.

- Update lib.py in the ~irods/scripts/irods. Change the get_hostname() function to
<pre><code>
    def get_hostname():
        host_name = socket.getaddrinfo( socket.gethostname(), 0, 0, 0, 0, socket.AI_CANONNAME)[0][3]
        if (len(host_name) > 64):
          return socket.gethostname()
        else:
         return host_name
</code></pre>

This change has been submitted as a pull request to the iRODS team.

- Check the hostname entry in the .irods/irods_environment.json file. Make sure the "irods-host" value is the same as the value of the hostname command in the shell.

- If you have a client machine to connect with an iRODS server running on azure, you will need to add an entry in /etc/hosts mapping the fully qualified domain name (longer than the iRODS max) to a shorter name. Note that this strategy has it's downside, in that the IP address of a machine inside the Azure cloud can change when the VM is restarted.  The implication is that production Azure machines should probably be configured with static IP addresses.

# Network configuration

The Azure network seems to want to tear down idle connections after 4 minutes.  This includes the iRODS control connection on port 1247. This has the unfortunate consequence of causing large file transfers to fail. On linux client machines that need to connect to iRODS servers inside of Azure, the following commands succesfully address the issue:
<pre><code>
sudo echo 120 > /proc/sys/net/ipv4/tcp_keepalive_time
sudo echo 30 > /proc/sys/net/ipv4/tcp_keepalive_intvl
</code></pre>

An issue suggesting the possibility of altering the iRODS socket code to address this problem has been submitted.

# Possibly helpful set of commands to install software pre-requisites on a fresh Azure vm
<pre><code>
sudo sudo apt-get install git
sudo apt-get install g++
sudo apt-get install libssl-dev
sudo apt-get install openssl-dev
sudo apt-get install glibmm24
sudo apt-get install libxml++-dev
sudo apt-get install irods-dev*
sudo apt-get install irods-externals*
sudo apt-get install glib
sudo apt-get install libglib2.0-dev
sudo apt-get install libglibmm-2.4-dev
sudo apt-get install libxml++2.6-2
sudo apt-get install libxml++
sudo apt-get install uuid-dev
</code></pre>
