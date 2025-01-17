﻿#include <iostream>
#include "main.h"
#include "nvme_util.h"
#include "int64.h"
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <fstream>
#include <linux/hdreg.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <iomanip>

#include <linux/mmc/ioctl.h>
#include <linux/major.h>

using namespace std;

const char *format_char_array(char *str, int strsize, const char *chr, int chrsize)
{
	int b = 0;
	while (b < chrsize && chr[b] == ' ')
		b++;
	int n = 0;
	while (b + n < chrsize && chr[b + n])
		n++;
	while (n > 0 && chr[b + n - 1] == ' ')
		n--;

	if (n >= strsize)
		n = strsize - 1;

	for (int i = 0; i < n; i++)
	{
		char c = chr[b + i];
		str[i] = (' ' <= c && c <= '~' ? c : '?');
	}
	str[n] = 0;
	return str;
}

static
const char *to_str(char(&str)[64], int k)
{
	if (!k)	// unsupported?
		str[0] = '-', str[1] = 0;
	else
		snprintf(str, sizeof(str), "%d Celsius", k - 273);
	return str;
}
const char *format_with_thousands_sep(char *str, int strsize, uint64_t val, const char *thousands_sep /*= 0 */)
{
	if (!thousands_sep)
	{
		thousands_sep = ",";
		#ifdef HAVE_LOCALE_H
		setlocale(LC_ALL, "");
		const struct lconv *currentlocale = localeconv();
		if (*(currentlocale->thousands_sep))
			thousands_sep = currentlocale->thousands_sep; 
		#endif
	}

	char num[64];
	snprintf(num, sizeof(num), "%"
		PRIu64, val);
	int numlen = strlen(num);

	int i = 0, j = 0;
	do
		str[j++] = num[i++];
	while (i < numlen && (numlen - i) % 3 != 0 && j < strsize - 1);
	str[j] = 0;

	while (i < numlen && j < strsize - 1)
	{
		j += snprintf(str + j, strsize - j, "%s%.3s", thousands_sep, num + i);
		i += 3;
	}

	return str;
}
const char *format_capacity(char *str, int strsize, uint64_t val, const char *decimal_point /*= 0 */)
{
	if (!decimal_point)
	{
		decimal_point = ".";
		#ifdef HAVE_LOCALE_H
		setlocale(LC_ALL, "");
		const struct lconv *currentlocale = localeconv();
		if (*(currentlocale->decimal_point))
			decimal_point = currentlocale->decimal_point;
		#endif
	}

	const unsigned factor = 1000;	// 1024 for KiB,MiB,...
	static
	const char prefixes[] = " KMGTP";

	// Find d with val in[d, d*factor)
	unsigned i = 0;
	uint64_t d = 1;
	for (uint64_t d2 = d * factor; val >= d2; d2 *= factor)
	{
		d = d2;
		if (++i >= sizeof(prefixes) - 2)
			break;
	}

	// Print 3 digits
	uint64_t n = val / d;
	if (i == 0)
		snprintf(str, strsize, "%u B", (unsigned) n);
	else if (n >= 100)	// "123 xB"
		snprintf(str, strsize, "%"
			PRIu64 " %cB", n, prefixes[i]);
	else if (n >= 10)	// "12.3 xB"
		snprintf(str, strsize, "%"
			PRIu64 "%s%u %cB", n, decimal_point,
			(unsigned)(((val % d) *10) / d), prefixes[i]);
	else	// "1.23 xB"
		snprintf(str, strsize, "%"
			PRIu64 "%s%02u %cB", n, decimal_point,
			(unsigned)(((val % d) *100) / d), prefixes[i]);

	return str;
}

static unsigned le16_to_uint(const unsigned char(&val)[2])
{
	return ((val[1] << 8) | val[0]);
}

static
const char *le128_to_str(char(&str)[64], uint64_t hi, uint64_t lo, unsigned bytes_per_unit)
{
	if (!hi)
	{
		// Up to 64-bit, print exact value
		format_with_thousands_sep(str, sizeof(str) - 16, lo);

		if (lo && bytes_per_unit && lo < 0xffffffffffffffffULL / bytes_per_unit)
		{
			int i = strlen(str);
			str[i++] = ' ';
			str[i++] = '[';
			format_capacity(str + i, (int) sizeof(str) - i - 1, lo *bytes_per_unit);
			i = strlen(str);
			str[i++] = ']';
			str[i] = 0;
		}
	}
	else
	{
		// More than 64-bit, print approximate value, prepend ~ flag
		snprintf(str, sizeof(str), "~%.0f",
			hi *(0xffffffffffffffffULL + 1.0) + lo);
	}

	return str;
}
// Format 128 bit LE integer for printing.
// Add value with SI prefixes if BYTES_PER_UNIT is specified.
static
const char *le128_to_str(char(&str)[64], const unsigned char(&val)[16],
		unsigned bytes_per_unit = 0)
{
	uint64_t hi = val[15];
	for (int i = 15 - 1; i >= 8; i--)
	{
		hi <<= 8;
		hi += val[i];
	}
	uint64_t lo = val[7];
	for (int i = 7 - 1; i >= 0; i--)
	{
		lo <<= 8;
		lo += val[i];
	}
	return le128_to_str(str, hi, lo, bytes_per_unit);
}

bool SCSI_id(int handle, unsigned char *id_buf)
{	
	//SG_IO structure
	sg_io_hdr io_hdr;
	unsigned char cdb[16];


	/*Prepare ATA command  data-in*/
	memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
	memset(&cdb, 0, sizeof(cdb));

	cdb[0] = 0xA1;
	cdb[1] = 0x08;
	cdb[2] = 0x0E;
	cdb[4] = 0x01;
	cdb[8] = 0xA0;
	cdb[9] = 0xEC;
	
	//io_hdr.flags = SG_FLAG_LUN_INHIBIT;
	io_hdr.interface_id = 'S';

	io_hdr.cmd_len = sizeof(cdb);
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;

	io_hdr.dxfer_len = 512;
	io_hdr.dxferp = id_buf;
	io_hdr.cmdp = cdb;
	io_hdr.timeout = 36000;

	if (ioctl(handle, SG_IO, &io_hdr) < 0)
	{
		cout << "ioctl failed! - Get SCSI identify" << endl;
		return false;
	}

	return true;
}

bool SCSI_smart(int handle, unsigned char *id_buf, int bufSize)
{	
	sg_io_hdr io_hdr;
	unsigned char cdb[16];

	memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
	memset(&cdb, 0, sizeof(cdb));

	cdb[0] = 0x85;
	cdb[1] = 0x08;
	cdb[2] = 0x0E;
	cdb[4] = 0xD0;
	cdb[6] = 0x01;
	cdb[10] = 0x4F;
	cdb[12] = 0xC2;
	cdb[14] = 0xB0;
	
	io_hdr.interface_id = 'S';

	io_hdr.cmd_len = sizeof(cdb);
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;

	io_hdr.dxfer_len = 512;
	io_hdr.dxferp = id_buf;
	io_hdr.cmdp = cdb;
	io_hdr.timeout = 36000;

	if (ioctl(handle, SG_IO, &io_hdr) < 0)
	{
		cout << "ioctl failed! - Get SCSI SMART" << endl;
		return false;
	}

	return true;

}

int main(int argc, char *argv[])
{
	std::vector<std::string > args;
	for (int i = 0; i < argc; i++)
	{
		args.push_back(argv[i]);
	}
	if (argc == 1)
	{
		ListAllDeviceInfo();
	}
	else if (argc == 2)
	{
        string version = "version 1.11.0";

		if (args[1].compare("-license") == 0)
		{
			string s1 = "========================================================================";
            string s2 = "SMARTQuery CMD";
			string s3 = "Copyright (c) 2021 Transcend Information, Inc. All rights reserved.";
			string s4 = "========================================================================";
			string s5 = "   End-User License Agreement (EULA)\n   \n   Software license terms\n   \n   Generally.\n   ------------------------------\n   Transcend Information, Inc. (\"Transcend\") is willing to grant the following license to install or\n   use the software and/or firmware (\"Licensed Software\") pursuant to this End-User License\n   Agreement (\"Agreement\"), whether provided separately or associated with a Transcend\n   product (\"Product\"), to the original purchaser of the Product upon or with which the Licensed\n   Software was installed or associated as of the time of purchase (\"Customer\") only if Customer\n   accepts all of the terms and conditions of this Agreement. PLEASE READ THESE TERMS\n   CAREFULLY. USING THE SOFTWARE WILL CONSTITUTE CUSTOMER'S ACCEPTANCE\n   OF THE TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE TO\n   THE TERMS AND CONDITIONS OF THIS AGREEMENT, DO NOT INSTALL OR USE THE\n   LICENSED SOFTWARE.\n   \n   License Grant.\n   ------------------------------\n   Transcend grants to Customer a personal, non-exclusive, non-transferable, non-distributable,\n   non-assignable, non-sublicensable license to install and use the Licensed Software on the\n   Product in accordance with the terms and conditions of this Agreement.\n   Intellectual Property Right.\n   As between Transcend and Customer, the copyright and all other intellectual property rights in\n   the Licensed Software are the property of Transcend or its supplier(s) or licensor(s). Any rights\n   not expressly granted in this License are reserved to Transcend.\n   \n   License Limitations.\n   ------------------------------\n   Customer may not, and may not authorize or permit any third party to: (a) use the Licensed\n   Software for any purpose other than in connection with the Product or in a manner inconsistent\n   with the design or documentations of the Licensed Software; (b) license, distribute, lease, rent,\n   lend, transfer, assign or otherwise dispose of the Licensed Software or use the Licensed\n   Software in any commercial hosted or service bureau environment; (c) reverse engineer,\n   decompile, disassemble or attempt to discover the source code for or any trade secrets related\n   to the Licensed Software, except and only to the extent that such activity is expressly\n   permitted by applicable law notwithstanding this limitation; (d) adapt, modify, alter, translate or\n   create any derivative works of the Licensed Software; (e) remove, alter or obscure any\n   copyright notice or other proprietary rights notice on the Licensed Software or Product; or (f)\n   circumvent or attempt to circumvent any methods employed by Transcend to control access to\n   the components, features or functions of the Product or Licensed Software.\n   \n   Copying.\n   ------------------------------\n   Customer may not copy the Licensed Software except that one copy of any separate software\n   component of the Licensed Software may be made to the extent that such copying is\n   necessary for Customer's own backup purposes.\n   Open Source.\n   The Licensed Software may contain open source components licensed to Transcend pursuant\n   to the license terms specified as below,\n   - GNU General Public License (GPL);\n   - GNU Lesser General Public License (LGPL);\n   - Apache License;\n   - MIT License;\n   - Berkeley Standard Distribution (BSD), the terms of which are currently available at;\n   and/or\n   - Code Project Open License (CPOL).\n   Customer may visit http://www.transcend-info.com/Legal/?no=10 to learn specifics of the open\n   source components contained in the Licensed Software and the respective license terms\n   thereof (\"Open Source License\"). In the event that this Agreement conflicts with the\n   requirements of the above one or more Open Source License with respect to the use of the\n   corresponding open source components, Customer agrees to be bound by such one or more\n   Open Source License.\n   \n   Disclaimer.\n   ------------------------------\n   TRANSCEND MAKES NO WARRANTY AND REPRESENTATIONS ABOUT THE\n   SUITABILITY, RELIABILITY, AVAILABILITY, TIMELINESS, LACK OF VIRUSES OR OTHER\n   HARMFUL COMPONENTS AND ACCURACY OF THE INFORMATION, LICENSED\n   SOFTWARE, PRODUCTS, SERVICES AND RELATED GRAPHICS CONTAINED WITHIN\n   THE LICENSED SOFTWARE FOR ANY PURPOSE. ALL SUCH INFORMATION, LICENSED\n   SOFTWARE, PRODUCTS, SERVICES AND RELATED GRAPHICS ARE PROVIDED \"AS IS\"\n   WITHOUT WARRANTY OF ANY KIND. TRANSCEND HEREBY DISCLAIMS ALL\n   WARRANTIES AND CONDITIONS WITH REGARD TO THIS INFORMATION, LICENSED\n   SOFTWARE, PRODUCTS, SERVICES AND RELATED GRAPHICS, INCLUDING ALL\n   IMPLIED WARRANTIES AND CONDITIONS OF MERCHANTABILITY, FITNESS FOR A\n   PARTICULAR PURPOSE, WORKMANLIKE EFFORT, TITLE, AND NON-INFRINGEMENT.\n   IN NO EVENT SHALL TRANSCEND BE LIABLE FOR ANY DIRECT, INDIRECT, PUNITIVE,\n   INCIDENTAL, SPECIAL, CONSEQUENTIAL DAMAGES OR ANY DAMAGES\n   WHATSOEVER INCLUDING, WITHOUT LIMITATION, DAMAGES FOR LOSS OF USE,\n   DATA OR PROFITS, ARISING OUT OF OR IN ANY WAY CONNECTION WITH THE USE,\n   PERFORMANCE OR ACCURACY OF THE LICENSED SOFTWARE OR WITH THE DELAY\n   OR INABILITY TO USE THE LICENSED SOFTWARE, OR THE PRODUCT WITH WHICH\n   THE LICENSED SOFTWARE IS ASSOCIATED, WHETHER BASED ON CONTRACT, TORT,\n   NEGLIGENCE, STRICT LIABILITY OR OTHERWISE, EVEN IF TRANSCEND HAS BEEN\n   ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.\n   \n   Limitation of Liability.\n   ------------------------------\n   IN ANY CASE, TRANSCEND 'S LIABILITY ARISING OUT OF OR IN CONNECTION WITH\n   THIS AGREEMENT WILL BE LIMITED TO THE TOTAL AMOUNT ACTUALLY AND\n   ORIGINALLY PAID AT RETAIL BY CUSTOMER FOR THE PRODUCT. The foregoing\n   Disclaimer and Limitation of Liability will apply to the maximum extent permitted by applicable\n   law. Some jurisdictions do not allow the exclusion or limitation of incidental or consequential\n   damages, so the exclusions and limitations set forth above may not apply.\n   \n   Termination.\n   ------------------------------\n   Transcend may, in addition to any other remedies available to Transcend, terminate this\n   Agreement immediately if Customer breaches any of its obligations under this Agreement.\n   \n   Miscellaneous.\n   (a) This Agreement constitutes the entire agreement between Transcend and Customer\n   concerning the subject matter hereof, and it may only be modified by a written amendment\n   signed by an authorized executive of Transcend. (b) Except to the extent applicable law, if any,\n   provides otherwise, this Agreement will be governed by the law of the Republic of China,\n   excluding its conflict of law provisions. (c) If any part of this Agreement is held invalid or\n   unenforceable, and the remaining portions will remain in full force and effect. (d) A waiver by\n   either party of any term or condition of this Agreement or any breach thereof, in any one\n   instance, will not waive such term or condition or any subsequent breach thereof. (e)\n   Transcend may assign its rights under this Agreement without condition. (f) This Agreement\n   will be binding upon and will inure to the benefit of the parties, their successors and permitted\n   assigns.\n   ";
			string s6 = "\n|-------------|\n| Statement   |\n|-------------|\n\nThis file incorporates open source software components subject to the terms of the copyright notice(s) and license agreement(s) below, and is provided \"as is\" WITHOUT WARRANTY OF ANY KIND, including but not limited to, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT.  \n\nYou may, at no charge, use, redistribute and modify such software, and download the source code for such software at http://www.transcend-info.com/Legal/?no=10.\n\nFurther, if applicable, under GNU General Public License and GNU Lesser General Public License, you may also obtain a CD containing the source code for such software by sending request to Transcend Information, Inc. (Legal and IPR Affairs Office/ No. 70, Xingzhong Rd., Neihu Dist., Taipei City 114, Taiwan, R.O.C.) at a fee to cover our cost of physical media and processing.  Your request must be sent within three (3) years of the date you received our product/software, and should include:\n - Your name\n - Your contact information\n - The name of our product/software (if you are simply requesting the source code for a specific component, please kindly name it)\n - The date you received our product/software\n\n\n******Copyright Information ******\nUbuntu\nVersion: 14.04 LTS\nAuthor: Mark Richard Shuttleworth (Canonical Ltd.)\nSource: http://www.ubuntu.com/download/alternative-downloads\nLicense: http://www.ubuntu.com/legal/terms-and-policies/intellectual-property-policy; http://www.ubuntu.com/about/about-ubuntu/licensing; http://www.gnu.org/licenses/gpl-2.0.html\n\n\n******A copy of each abovementioned license is displayed below ******\nUBUNTU V Intellectual property rights?policy\nLatest update: We updated this policy on 15 July 2015.\nWelcome to Canonicals IPRights Policy. This policy is published by Canonical Limited (Canonical, we, us and our) under the Creative Commons CC-BY-SA version 3.0 UK licence.\nCanonical owns and manages certain intellectual property rights in Ubuntu and other associated intellectual property (Canonical IP) and licenses the use of these rights to enterprises, individuals and members of the Ubuntu community in accordance with this IPRights Policy.\nYour use of Canonical IP is subject to:\n - Your acceptance of this IPRights Policy;\n - Your acknowledgement that Canonical IP is the exclusive property of Canonical and can only be used with Canonicals permission (which can be revoked at any time); and\n - You taking all reasonable steps to ensure that Canonical IP is used in a manner that does not affect either the validity of such Canonical IP or Canonicals ownership of Canonical IP in any way; and that you will transfer any goodwill you derive from them to Canonical, if requested.\nUbuntu is a trusted open source platform. To maintain that trust we need to manage the use of Ubuntu and the components within it very carefully. This way, when people use Ubuntu, or anything bearing the Ubuntu brand, they can be assured that it will meet the standards they expect. Your continued use of Canonical IP implies your acceptance and acknowledgement of this IPRights Policy.\n\n1. Summary\n----------------------------------------\n - You can download, install and receive updates to Ubuntu for free.\n - You can modify Ubuntu for personal or internal commercial use.\n - You can redistribute Ubuntu, but only where there has been no modification to it.\n - You can use our copyright, patent and design materials in accordance with this IPRights Policy.\n - You can be confident and can trust in the consistency of the Ubuntu experience.\n - You can rely on the standard expected of Ubuntu.\n - Ubuntu is an aggregate work; this policy does not modify or reduce rights granted under licences which apply to specific works in Ubuntu.\n\n2. Relationship to other licences\n----------------------------------------\nUbuntu is an aggregate work of many works, each covered by their own licence(s). For the purposes of determining what you can do with specific works in Ubuntu, this policy should be read together with the licence(s) of the relevant packages. For the avoidance of doubt, where any other licence grants rights, this policy does not modify or reduce those rights under those licences.\n\n3. Your use of Ubuntu\n----------------------------------------\n - You can download, install and receive updates to Ubuntu for free.\n - Ubuntu is freely available to all users for personal, or in the case of organisations, internal use. It is provided for this use without warranty. All implied warranties are disclaimed to the fullest extent permitted at law.\n - You can modify Ubuntu for personal or internal use\n - You can make changes to Ubuntu for your own personal use or for your organisations own internal use.\n - You can redistribute Ubuntu, but only where there has been no modification to it.\n - You can redistribute Ubuntu in its unmodified form, complete with the installer images and packages provided by Canonical (this includes the publication or launch of virtual machine images).\n - Any redistribution of modified versions of Ubuntu must be approved, certified or provided by Canonical if you are going to associate it with the Trademarks. Otherwise you must remove and replace the Trademarks and will need to recompile the source code to create your own binaries. This does not affect your rights under any open source licence applicable to any of the components of Ubuntu. If you need us to approve, certify or provide modified versions for redistribution you will require a licence agreement from Canonical, for which you may be required to pay. For further information, please contact us (as set out below).\n - We do not recommend using modified versions of Ubuntu which are not modified in accordance with this IPRights Policy. Modified versions may be corrupted and users of such modified systems or images may find them to be inconsistent with the updates published by Canonical to its users. If they use the Trademarks, they are in contravention of this IPRights Policy. Canonical cannot guarantee the performance of such modified versions. Canonicals updates will be consistent with every version of Ubuntu approved, certified or provided by Canonical.\n\n4. Your use of our trademarks\n----------------------------------------\nCanonicals Trademarks (registered in word and logo form) include:\nUBUNTU\nKUBUNTU\nEDUBUNTU\nXUBUNTU\nJUJU\nLANDSCAPE\n - You can use the Trademarks, in accordance with Canonicals brand guidelines, with Canonicals permission in writing. If you require a Trademark licence, please contact us (as set out below).\n - You will require Canonicals permission to use: (i) any mark ending with the letters UBUNTU or BUNTU which is sufficiently similar to the Trademarks or any other confusingly similar mark, and (ii) any Trademark in a domain name or URL or for merchandising purposes.\n - You cannot use the Trademarks in software titles. If you are producing software for use with or on Ubuntu you may reference Ubuntu, but must avoid: (i) any implication of endorsement, or (ii) any attempt to unfairly or confusingly capitalise on the goodwill of Canonical or Ubuntu.\n - You can use the Trademarks in discussion, commentary, criticism or parody, provided that you do not imply endorsement by Canonical.\n - You can write articles, create websites, blogs or talk about Ubuntu, provided that it is clear that you are in no way speaking for or on behalf of Canonical and that you do not imply endorsement by Canonical.\nCanonical reserves the right to review all use of Canonicals Trademarks and to object to any use that appears outside of this IPRights Policy.\n\n5. Your use of our copyright, patent and design materials\n-------------------------------------------------------------\n - You can only use Canonicals copyright materials in accordance with the copyright licences therein and this IPRights Policy.\n - You cannot use Canonicals patented materials without our permission.\n*Copyright:\nThe disk, CD, installer and system images, together with Ubuntu packages and binary files, are in many cases copyright of Canonical (which copyright may be distinct from the copyright in the individual components therein) and can only be used in accordance with the copyright licences therein and this IPRights Policy.\n*Patents:\nCanonical has made a significant investment in the Open Invention Network, defending Linux, for the benefit of the open source ecosystem. Additionally, like many open source projects, Canonical also protects its interests from third parties by registering patents. You cannot use Canonicals patented materials without our permission.\n*Trade dress and look and feel:\nCanonical owns intellectual property rights in the trade dress and look and feel of Ubuntu (including the Unity interface), along with various themes and components that may include unregistered design rights, registered design rights and design patents, your use of Ubuntu is subject to these rights.\n\n6. Logo use guidelines\n----------------------------------------\nCanonicals logos are presented in multiple colours and it is important that their visual integrity be maintained. It is therefore preferable that the logos should only be used in their standard form, but if you should feel the need to alter them in any way, you should following the guidelines set out below.\n - Ubuntu logo guidelines\n - Canonical logo guidelines\n\n7. Use of Canonical IP by the Ubuntu community\n---------------------------------------------------\nUbuntu is built by Canonical and the Ubuntu community. We share access rights owned by Canonical with the Ubuntu community for the purposes of discussion, development and advocacy. We recognise that most of the open source discussion and development areas are for non-commercial purposes and we therefore allow the use of Canonical IP in this context, as long as there is no commercial use and that the Canonical IP is used in accordance with this IPRights Policy.\n\n8. Contact us\n----------------------------------------\nPlease contact us:\n - if you have any questions or would like further information on our IPRights Policy, Canonical or Canonical IP;\n - if you would like permission from Canonical to use Canonical IP;\n - if you require a licence agreement; or\n - to report a breach of our IPRights Policy.\nPlease note that due to the volume of mail we receive, it may take up to a week to process your request.\n\n9. Changes\n----------------------------------------\nWe may make changes to this IPRights Policy from time to time. Please check this IPRights Policy from time to time to ensure that you are in compliance.\n------\n=====================\nUBUNTU - Licensing\n=====================\nUbuntu is a collection of thousands of computer programs and documents created by a range of individuals, teams and companies.\nEach of these programs may come under a different licence. This licence policy describes the process that we follow in determining which software will be included by default in the Ubuntu operating system.\nCopyright licensing and trademarks are two different areas of law, and we consider them separately in Ubuntu. The following policy applies only to copyright licences. We evaluate trademarks on a case-by-case basis.\nCategories of software in Ubuntu\nThe thousands of software packages available for Ubuntu are organised into four key groups or components: main, restricted, universe and multiverse. Software is published in one of these components based on whether or not it meets our free software philosophy, and the level of support we can provide for it.\nThis policy only addresses the software that you will find in main and restricted, which contain software that is fully supported by the Ubuntu team and must comply with this policy.\nUbuntu 'main' component licence policy\nAll application software included in the Ubuntu main component:\n - Must include source code. The main component has a strict and non-negotiable requirement that application software included in it must come with full source code.\n - Must allow modification and distribution of modified copies under the same licence. Just having the source code does not convey the same freedom as having the right to change it. Without the ability to modify software, the Ubuntu community cannot support software, fix bugs, translate it, or improve it.\nUbuntu 'main' and 'restricted' component licence policy\nAll application software in both main and restricted must meet the following requirements:\n - Must allow redistribution. Your right to sell or give away the software alone, or as part of an aggregate software distribution, is important because:\n --- You, the user, must be able to pass on any software you have received from Ubuntu in either source code or compiled form.\n --- While Ubuntu will not charge licence fees for this distribution, you might want to charge to print Ubuntu CDs, or create your own customised versions of Ubuntu which you sell, and should have the freedom to do so.\n - Must not require royalty payments or any other fee for redistribution or modification.It's important that you can exercise your rights to this software without having to pay for the privilege, and that you can pass these rights on to other people on exactly the same basis.\n - Must allow these rights to be passed on along with the software. You should be able to have exactly the same rights to the software as we do.\n - Must not discriminate against persons, groups or against fields of endeavour. The licence of software included in Ubuntu can not discriminate against anyone or any group of users and cannot restrict users from using the software for a particular field of endeavour - a business for example. So we will not distribute software that is licensed \"freely for non-commercial use\".\n - Must not be distributed under a licence specific to Ubuntu. The rights attached to the software must not depend on the program being part of Ubuntu system. So we will not distribute software for which Ubuntu has a \"special\" exemption or right, and we will not put our own software into Ubuntu and then refuse you the right to pass it on.\n - Must not contaminate other software licences.The licence must not place restrictions on other software that is distributed along with it. For example, the licence must not insist that all other programmes distributed on the same medium be free software.\n - May require source modifications to be distributed as patches. In some cases, software authors are happy for us to distribute their software and modifications to their software, as long as the two are distributed separately, so that people always have a copy of their pristine code. We are happy to respect this preference. However, the licence must explicitly permit distribution of software built from modified source code.\nDocumentation, firmware and drivers\nUbuntu contains licensed and copyrighted works that are not application software. For example, the default Ubuntu installation includes documentation, images, sounds, video clips and firmware. The Ubuntu community will make decisions on the inclusion of these works on a case-by-case basis, ensuring that these works do not restrict our ability to make Ubuntu available free of charge, and that you can continue to redistribute Ubuntu.\nSoftware installed by default\nWhen you install Ubuntu, you will typically install a complete desktop environment. It is also possible to install a minimal set of software (just enough to boot your machine) and then manually select the precise software applications to install. Such a \"custom\" install is usually favoured by server administrators, who prefer to keep only the software they absolutely need on the server.\nAll of the application software installed by default is free software. In addition, we install some hardware drivers that are available only in binary format, but such packages are clearly marked in the restricted component.\n------\n\n============================\nGNU GENERAL PUBLIC LICENSE\n============================\nVersion 2, June 1991\nCopyright (C) 1989, 1991 Free Software Foundation, Inc.\n51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA\n\nEveryone is permitted to copy and distribute verbatim copies of this license document, but changing it is not allowed.\nPreamble:\nThe licenses for most software are designed to take away your freedom to share and change it. By contrast, the GNU General Public License is intended to guarantee your freedom to share and change free software--to make sure the software is free for all its users. This General Public License applies to most of the Free Software Foundation's software and to any other program whose authors commit to using it. (Some other Free Software Foundation software is covered by the GNU Lesser General Public License instead.) You can apply it to your programs, too. \nWhen we speak of free software, we are referring to freedom, not price. Our General Public Licenses are designed to make sure that you have the freedom to distribute copies of free software (and charge for this service if you wish), that you receive source code or can get it if you want it, that you can change the software or use pieces of it in new free programs; and that you know you can do these things. \nTo protect your rights, we need to make restrictions that forbid anyone to deny you these rights or to ask you to surrender the rights. These restrictions translate to certain responsibilities for you if you distribute copies of the software, or if you modify it. \nFor example, if you distribute copies of such a program, whether gratis or for a fee, you must give the recipients all the rights that you have. You must make sure that they, too, receive or can get the source code. And you must show them these terms so they know their rights. \nWe protect your rights with two steps: (1) copyright the software, and (2) offer you this license which gives you legal permission to copy, distribute and/or modify the software. \nAlso, for each author's protection and ours, we want to make certain that everyone understands that there is no warranty for this free software. If the software is modified by someone else and passed on, we want its recipients to know that what they have is not the original, so that any problems introduced by others will not reflect on the original authors' reputations. \nFinally, any free program is threatened constantly by software patents. We wish to avoid the danger that redistributors of a free program will individually obtain patent licenses, in effect making the program proprietary. To prevent this, we have made it clear that any patent must be licensed for everyone's free use or not licensed at all. \nThe precise terms and conditions for copying, distribution and modification follow. \nTERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION:\n0. This License applies to any program or other work which contains a notice placed by the copyright holder saying it may be distributed under the terms of this General Public License. The \"Program\", below, refers to any such program or work, and a \"work based on the Program\" means either the Program or any derivative work under copyright law: that is to say, a work containing the Program or a portion of it, either verbatim or with modifications and/or translated into another language. (Hereinafter, translation is included without limitation in the term \"modification\".) Each licensee is addressed as \"you\". \nActivities other than copying, distribution and modification are not covered by this License; they are outside its scope. The act of running the Program is not restricted, and the output from the Program is covered only if its contents constitute a work based on the Program (independent of having been made by running the Program). Whether that is true depends on what the Program does. \n1. You may copy and distribute verbatim copies of the Program's source code as you receive it, in any medium, provided that you conspicuously and appropriately publish on each copy an appropriate copyright notice and disclaimer of warranty; keep intact all the notices that refer to this License and to the absence of any warranty; and give any other recipients of the Program a copy of this License along with the Program. \nYou may charge a fee for the physical act of transferring a copy, and you may at your option offer warranty protection in exchange for a fee. \n2. You may modify your copy or copies of the Program or any portion of it, thus forming a work based on the Program, and copy and distribute such modifications or work under the terms of Section 1 above, provided that you also meet all of these conditions: \na) You must cause the modified files to carry prominent notices stating that you changed the files and the date of any change. \nb) You must cause any work that you distribute or publish, that in whole or in part contains or is derived from the Program or any part thereof, to be licensed as a whole at no charge to all third parties under the terms of this License. \nc) If the modified program normally reads commands interactively when run, you must cause it, when started running for such interactive use in the most ordinary way, to print or display an announcement including an appropriate copyright notice and a notice that there is no warranty (or else, saying that you provide a warranty) and that users may redistribute the program under these conditions, and telling the user how to view a copy of this License. (Exception: if the Program itself is interactive but does not normally print such an announcement, your work based on the Program is not required to print an announcement.) \nThese requirements apply to the modified work as a whole. If identifiable sections of that work are not derived from the Program, and can be reasonably considered independent and separate works in themselves, then this License, and its terms, do not apply to those sections when you distribute them as separate works. But when you distribute the same sections as part of a whole which is a work based on the Program, the distribution of the whole must be on the terms of this License, whose permissions for other licensees extend to the entire whole, and thus to each and every part regardless of who wrote it. \nThus, it is not the intent of this section to claim rights or contest your rights to work written entirely by you; rather, the intent is to exercise the right to control the distribution of derivative or collective works based on the Program. \nIn addition, mere aggregation of another work not based on the Program with the Program (or with a work based on the Program) on a volume of a storage or distribution medium does not bring the other work under the scope of this License. \n3. You may copy and distribute the Program (or a work based on it, under Section 2) in object code or executable form under the terms of Sections 1 and 2 above provided that you also do one of the following: \na) Accompany it with the complete corresponding machine-readable source code, which must be distributed under the terms of Sections 1 and 2 above on a medium customarily used for software interchange; or, \nb) Accompany it with a written offer, valid for at least three years, to give any third party, for a charge no more than your cost of physically performing source distribution, a complete machine-readable copy of the corresponding source code, to be distributed under the terms of Sections 1 and 2 above on a medium customarily used for software interchange; or, \nc) Accompany it with the information you received as to the offer to distribute corresponding source code. (This alternative is allowed only for noncommercial distribution and only if you received the program in object code or executable form with such an offer, in accord with Subsection b above.) \nThe source code for a work means the preferred form of the work for making modifications to it. For an executable work, complete source code means all the source code for all modules it contains, plus any associated interface definition files, plus the scripts used to control compilation and installation of the executable. However, as a special exception, the source code distributed need not include anything that is normally distributed (in either source or binary form) with the major components (compiler, kernel, and so on) of the operating system on which the executable runs, unless that component itself accompanies the executable. \nIf distribution of executable or object code is made by offering access to copy from a designated place, then offering equivalent access to copy the source code from the same place counts as distribution of the source code, even though third parties are not compelled to copy the source along with the object code. \n4. You may not copy, modify, sublicense, or distribute the Program except as expressly provided under this License. Any attempt otherwise to copy, modify, sublicense or distribute the Program is void, and will automatically terminate your rights under this License. However, parties who have received copies, or rights, from you under this License will not have their licenses terminated so long as such parties remain in full compliance. \n5. You are not required to accept this License, since you have not signed it. However, nothing else grants you permission to modify or distribute the Program or its derivative works. These actions are prohibited by law if you do not accept this License. Therefore, by modifying or distributing the Program (or any work based on the Program), you indicate your acceptance of this License to do so, and all its terms and conditions for copying, distributing or modifying the Program or works based on it. \n6. Each time you redistribute the Program (or any work based on the Program), the recipient automatically receives a license from the original licensor to copy, distribute or modify the Program subject to these terms and conditions. You may not impose any further restrictions on the recipients' exercise of the rights granted herein. You are not responsible for enforcing compliance by third parties to this License. \n7. If, as a consequence of a court judgment or allegation of patent infringement or for any other reason (not limited to patent issues), conditions are imposed on you (whether by court order, agreement or otherwise) that contradict the conditions of this License, they do not excuse you from the conditions of this License. If you cannot distribute so as to satisfy simultaneously your obligations under this License and any other pertinent obligations, then as a consequence you may not distribute the Program at all. For example, if a patent license would not permit royalty-free redistribution of the Program by all those who receive copies directly or indirectly through you, then the only way you could satisfy both it and this License would be to refrain entirely from distribution of the Program. \nIf any portion of this section is held invalid or unenforceable under any particular circumstance, the balance of the section is intended to apply and the section as a whole is intended to apply in other circumstances. \nIt is not the purpose of this section to induce you to infringe any patents or other property right claims or to contest validity of any such claims; this section has the sole purpose of protecting the integrity of the free software distribution system, which is implemented by public license practices. Many people have made generous contributions to the wide range of software distributed through that system in reliance on consistent application of that system; it is up to the author/donor to decide if he or she is willing to distribute software through any other system and a licensee cannot impose that choice. \nThis section is intended to make thoroughly clear what is believed to be a consequence of the rest of this License. \n8. If the distribution and/or use of the Program is restricted in certain countries either by patents or by copyrighted interfaces, the original copyright holder who places the Program under this License may add an explicit geographical distribution limitation excluding those countries, so that distribution is permitted only in or among countries not thus excluded. In such case, this License incorporates the limitation as if written in the body of this License. \n9. The Free Software Foundation may publish revised and/or new versions of the General Public License from time to time. Such new versions will be similar in spirit to the present version, but may differ in detail to address new problems or concerns. \nEach version is given a distinguishing version number. If the Program specifies a version number of this License which applies to it and \"any later version\", you have the option of following the terms and conditions either of that version or of any later version published by the Free Software Foundation. If the Program does not specify a version number of this License, you may choose any version ever published by the Free Software Foundation. \n10. If you wish to incorporate parts of the Program into other free programs whose distribution conditions are different, write to the author to ask for permission. For software which is copyrighted by the Free Software Foundation, write to the Free Software Foundation; we sometimes make exceptions for this. Our decision will be guided by the two goals of preserving the free status of all derivatives of our free software and of promoting the sharing and reuse of software generally. \nNO WARRANTY:\n11. BECAUSE THE PROGRAM IS LICENSED FREE OF CHARGE, THERE IS NO WARRANTY FOR THE PROGRAM, TO THE EXTENT PERMITTED BY APPLICABLE LAW. EXCEPT WHEN OTHERWISE STATED IN WRITING THE COPYRIGHT HOLDERS AND/OR OTHER PARTIES PROVIDE THE PROGRAM \"AS IS\" WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE OF THE PROGRAM IS WITH YOU. SHOULD THE PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF ALL NECESSARY SERVICING, REPAIR OR CORRECTION. \n12. IN NO EVENT UNLESS REQUIRED BY APPLICABLE LAW OR AGREED TO IN WRITING WILL ANY COPYRIGHT HOLDER, OR ANY OTHER PARTY WHO MAY MODIFY AND/OR REDISTRIBUTE THE PROGRAM AS PERMITTED ABOVE, BE LIABLE TO YOU FOR DAMAGES, INCLUDING ANY GENERAL, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OR INABILITY TO USE THE PROGRAM (INCLUDING BUT NOT LIMITED TO LOSS OF DATA OR DATA BEING RENDERED INACCURATE OR LOSSES SUSTAINED BY YOU OR THIRD PARTIES OR A FAILURE OF THE PROGRAM TO OPERATE WITH ANY OTHER PROGRAMS), EVEN IF SUCH HOLDER OR OTHER PARTY HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES. \nEND OF TERMS AND CONDITIONS:\nHow to Apply These Terms to Your New Programs:\nIf you develop a new program, and you want it to be of the greatest possible use to the public, the best way to achieve this is to make it free software which everyone can redistribute and change under these terms.\nTo do so, attach the following notices to the program. It is safest to attach them to the start of each source file to most effectively convey the exclusion of warranty; and each file should have at least the \"copyright\" line and a pointer to where the full notice is found.\none line to give the program's name and an idea of what it does.\nCopyright (C) yyyy  name of author\n\nThis program is free software; you can redistribute it and/or\nmodify it under the terms of the GNU General Public License\nas published by the Free Software Foundation; either version 2\nof the License, or (at your option) any later version.\n\nThis program is distributed in the hope that it will be useful,\nbut WITHOUT ANY WARRANTY; without even the implied warranty of\nMERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\nGNU General Public License for more details.\n\nYou should have received a copy of the GNU General Public License\nalong with this program; if not, write to the Free Software\nFoundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\nAlso add information on how to contact you by electronic and paper mail.\nIf the program is interactive, make it output a short notice like this when it starts in an interactive mode:\nGnomovision version 69, Copyright (C) year name of author\nGnomovision comes with ABSOLUTELY NO WARRANTY; for details\ntype `show w'.  This is free software, and you are welcome\nto redistribute it under certain conditions; type `show c' \nfor details.\nThe hypothetical commands `show w' and `show c' should show the appropriate parts of the General Public License. Of course, the commands you use may be called something other than `show w' and`show c'; they could even be mouse-clicks or menu items--whatever suits your program.\nYou should also get your employer (if you work as a programmer) or your school, if any, to sign a \"copyright disclaimer\" for the program, if necessary. Here is a sample; alter the names:\nYoyodyne, Inc., hereby disclaims all copyright\ninterest in the program `Gnomovision'\n(which makes passes at compilers) written \nby James Hacker.\n\nsignature of Ty Coon, 1 April 1989\nTy Coon, President of Vice\nThis General Public License does not permit incorporating your program into proprietary programs. If your program is a subroutine library, you may consider it more useful to permit linking proprietary applications with the library. If this is what you want to do, use the GNU Lesser General Public Licenseinstead of this License.\n------\n";

			cout << s1 << endl;
			cout << s2 << endl;
			cout << s3 << endl;
			cout << s4 << endl;
			cout << s5 << endl;
			cout << s6 << endl;
		}
		else if (args[1].compare("-v") == 0)
		{
			cout << version << endl;
		}
		else
		{
			showGuide();
		}
	}
	else if (args[1].find("-") == 0 && argc >= 3)
	{
		if(isSupport(args[2])==0)
		{
			return 0;
		}	   
		if (args[1].compare("-smart") == 0)
		{
			if (args[2].find("nvme") != std::string::npos)
			{
				DumpNVMESMARTInfo(args[2]);
			}
			else
			{
				DumpSATASCSI_SMARTInfo(args[2]);
			}
		}
		else if (args[1].compare("-id") == 0)
		{
			if (args[2].find("nvme") != std::string::npos)
			{
				DumpNVMEIDInfo(args[2]);
			}
			else
			{
				DumpSATASCSI_IDInfo(args[2]);
			}
		}
		else if (args[1].compare("-health") == 0)
		{

			if (args[2].find("nvme") != std::string::npos)
			{
				DumpNVMEHealthInfo(args[2]);
			}
			else
			{
				DumpSATASCSI_HealthInfo(args[2]);
			}
		}
		else if (args[1].compare("-all") == 0)
		{
			if (args[2].find("nvme") != std::string::npos)
			{
				DumpAllNVMEInfo(args[2]);
			}
			else
			{
				DumpSATASCSI_AllInfo(args[2]);
			}
		}
		else
		{
			showGuide();
		}
	}
	else
	{
		showGuide();
	}
}

void ListAllDeviceInfo()
{
	char device_num = 'a';
	string str;
	string byte;
	/* Start of "Scan ATA Device" loop */
	for (int i = 0; i < 26; i++)
	{
		string device_str_byte = "/dev/sd";
		device_str_byte += device_num;
		if(isSupport(device_str_byte)!=0)
		{ 
			DumpSATASCSI_AllInfo(device_str_byte);
		}
		device_num = device_num + 1;
		
  	}

	/*start of "Scan NVME device" loop */
	for (int x = 0; x < 10; x++)
	{
		for (int y = 0; y < 10; y++)
		{
			string tmp_str = "/dev/nvme" + std::to_string(x) + "n" + std::to_string(y);
			if(isSupport(tmp_str)!=0)
			{
				DumpNVMEIDInfo(tmp_str);
				DumpNVMESMARTInfo(tmp_str);
				DumpNVMEHealthInfo(tmp_str);
			}
	
		}
	}
}
void DumpSATASCSI_AllInfo(string deviceStr)
{
	int ok = 0;
	ok += DumpSATASCSI_IDInfo(deviceStr);
	if (ok == 0)
	{
		ok += DumpSATASCSI_SMARTInfo(deviceStr);
		ok += DumpSATASCSI_HealthInfo(deviceStr);
	}
}

void DumpAllNVMEInfo(string deviceStr)
{
	int ok = 0;
	ok += DumpNVMEIDInfo(deviceStr);
	ok += DumpNVMESMARTInfo(deviceStr);
	ok += DumpNVMEHealthInfo(deviceStr);
}

int DumpNVMEBadblock(string deviceStr)
{
	nvme_Device * nvmeDev;
	nvmeDev = new nvme_Device(deviceStr.c_str(), "", 0);
	int fd = nvmeDev->myOpen();
	if (fd < 0)
		return -1;
	char buf[64];
	nvme_Device::nvme_badblock bad_ctrl;
	nvmeDev->nvme_read_badblock(&bad_ctrl, sizeof(bad_ctrl));
	int res = bad_ctrl.badblock[0];
	string s1 = "bad block\t: " + std::to_string(res);
	cout << s1 << endl;
	return 0;
}

int DumpNVMEWAI(string deviceStr)
{
	nvme_Device * nvmeDev;
	nvmeDev = new nvme_Device(deviceStr.c_str(), "", 0);
	int fd = nvmeDev->myOpen();
	if (fd < 0)
		return -1;
	char buf[64];
	nvme_Device::nvme_WAI_Info wai_info;
	int res = nvmeDev->nvme_read_WAI(&wai_info, sizeof(wai_info));
	float waitlc = wai_info.wai[1008];
	float waislc = wai_info.wai[1016];
	string s1 = "WAI TLC\t: "  ;
	string s2 = "WAI SLC\t: " ;
	cout <<s1;
	cout <<setprecision(2)<< waitlc/100 << endl;
	cout << s2  ;
 	cout <<setprecision(2)<< waislc/100 << endl;
 	return 0;
	 
}

int DumpSATASCSI_IDInfo(string deviceStr)
{
	std::string model;
	std::string fw_ver;
	std::string serial_no;

	struct hd_driveid hd;
	int fd;
	memset(&hd, 0, 512);
	fd = ::open(deviceStr.c_str(), O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return -1;
		
	bool isSCSI = false; 
	
	int ret = ioctl(fd, HDIO_GET_IDENTITY, &hd);
	::close(fd);
	if(ret == 0)  //SATA
	{
		model = reinterpret_cast<char*> (hd.model);
		fw_ver = reinterpret_cast<char*> (hd.fw_rev);
		serial_no = reinterpret_cast<char*> (hd.serial_no);
	}
	else   //SCSI
	{
		memset(&hd, 0, 512);
		unsigned char idTable[512];
		int sg_fd;
		sg_fd = ::open(deviceStr.c_str(), O_RDWR);
		if (sg_fd < 0)
			return -1;	
	
		if(SCSI_id(sg_fd, idTable))
		{
			memcpy(&hd, idTable, sizeof(hd_driveid));
			
			
			model = reinterpret_cast<char*> (hd.model);
			fw_ver = reinterpret_cast<char*> (hd.fw_rev);
			serial_no = reinterpret_cast<char*> (hd.serial_no);
			
			
			string model_rev = "";
			int i = 0;
			while(model[i] != '\0')
			{
				model_rev += model[i+1];
				model_rev += model[i];
				i+=2;	
			}
			model = model_rev;
			
			string fw_rev = "";
			i = 0;
			while(fw_ver[i] != '\0')
			{
				fw_rev += fw_ver[i+1];
				fw_rev += fw_ver[i];
				i+=2;	
			}
			fw_ver = fw_rev;
			
			string serial_rev = "";
			i = 0;
			while(serial_no[i] != '\0')
			{
				serial_rev += serial_no[i+1];
				serial_rev += serial_no[i];
				i+=2;	
			}
			serial_no = serial_rev;
			isSCSI = true;
	 	}
	 	else
	 	{
	 		::close(sg_fd);
	 		return -1;
	 	}
	 	::close(sg_fd);
	}
	
	
 	string s1 = "---------- Disk Information ----------";
	cout << s1 << endl;

	string s2 = "Model\t\t\t:" + model.substr(0, 40);
	cout << s2 << endl;
	string s3 = "FW Version\t\t:" + fw_ver.substr(0, 8);
	cout << s3 << endl;
	string s4 = "Serial No\t\t:" + serial_no.substr(0, 20);
	cout << s4 << endl;
	string s5 = "";
	if(isSCSI)
	{
		s5 = "Support Interface\t:SCSI";
	}
	else
	{
		s5 = "Support Interface\t:SATA";
	}
	cout << s5 << endl;
	
	
	return 0;

}

int DumpNVMEIDInfo(string deviceStr)
{
	nvme_Device * nvmeDev;
	nvmeDev = new nvme_Device(deviceStr.c_str(), "", 0);
	int fd = nvmeDev->myOpen();
	if (fd < 0)
		return -1;
	char buf[64];
	nvme_Device::nvme_id_ctrl id_ctrl;
	const char *model_str_byte;
	const char *serialno_str_byte;
	const char *
		fwrev_str_byte;
	nvmeDev->nvme_read_id_ctrl(id_ctrl);

	string s1 = "---------- Disk Information ----------";
	cout << s1 << endl;
	model_str_byte = format_char_array(buf, id_ctrl.mn);
	string s2 = "Model\t\t\t:" + std::string(model_str_byte);
	cout << s2 << endl;

	fwrev_str_byte = format_char_array(buf, id_ctrl.fr);
	string s3 = "FW Version\t\t:" + std::string(fwrev_str_byte);
	cout << s3 << endl;

	serialno_str_byte = format_char_array(buf, id_ctrl.sn);
	string s4 = "Serial No\t\t:" + std::string(serialno_str_byte);
	cout << s4 << endl;

	string s5 = "Support Interface\t:NVME";
	cout << s5 << endl;

	::close(fd);
	return 0;
}
vector<string> get_SMART_Attributes(int fd)
{
 	vector<string> attributesStringList;
	
	attributesStringList.push_back("01 Raw Read Error Rate");
	attributesStringList.push_back("05 Reallocated Sectors Count");
	attributesStringList.push_back("09 Power-On Hours");
	attributesStringList.push_back("0C Power Cycle Count");
	attributesStringList.push_back("A0 Uncorrectable Sector Count");
	attributesStringList.push_back("A1 Valid Spare Blocks");
	attributesStringList.push_back("A3 Initial Invalid Blocks");
	attributesStringList.push_back("A4 Total TLC Erase Count");
	attributesStringList.push_back("A5 Maximum TLC Erase Count");
	attributesStringList.push_back("A6 Minimum TLC Erase Count");
	attributesStringList.push_back("A7 Average TLC Erase Count");
	attributesStringList.push_back("A8 Vendor Specific");
	attributesStringList.push_back("A9 Percentage Lifetime Remaining");
	attributesStringList.push_back("AF Vendor Specific");
	attributesStringList.push_back("B0 Vendor Specific");
	attributesStringList.push_back("B1 Vendor Specific");
	attributesStringList.push_back("B2 Vendor Specific");
	attributesStringList.push_back("B5 Program Fail Count");
	attributesStringList.push_back("B6 Erase Fail Count");
	attributesStringList.push_back("C0 Power-off Retract Count");
	attributesStringList.push_back("C2 Temperature");
	attributesStringList.push_back("C3 Cumulative ECC Bit Correction Count");
	attributesStringList.push_back("C4 Reallocation Event Count");
	attributesStringList.push_back("C5 Current Pending Sector Count");
	attributesStringList.push_back("C6 Smart Off-line Scan Uncorrectable Error Count");
	attributesStringList.push_back("C7 Ultra DMA CRC Error Rate");
	attributesStringList.push_back("E8 Available Reserved Space");
	attributesStringList.push_back("F1 Total LBA Write");
	attributesStringList.push_back("F2 Total LBA Read");
	attributesStringList.push_back("F5 Cumulative Program NAND Pages");
	return attributesStringList;
}
int DumpSATASCSI_SMARTInfo(string deviceStr)
{
	int smartOffset = 0;
	int fd;
	int buf_size = 512;
	fd = ::open(deviceStr.c_str(), O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	char id_arr[3];
	unsigned char buf[buf_size] = { WIN_SMART, 0, SMART_READ_VALUES, 1
	};
	int ret = ioctl(fd, HDIO_DRIVE_CMD, (unsigned char *) buf);
	
	if(ret != 0)  //SCSI
	{
		smartOffset = 4;
		memset(&buf, 0, buf_size);
		unsigned char varBuf[512];
		int sg_fd;
		sg_fd = ::open(deviceStr.c_str(), O_RDWR);
		if (sg_fd < 0)
			return false;	
		if(SCSI_smart(sg_fd, varBuf, 512)){
			memcpy(&buf, varBuf, sizeof(buf));
		}
		::close(sg_fd);
	}
	
	

	vector<string> attributesStringList = get_SMART_Attributes(fd);
	string s1 = "---------------- S.M.A.R.T Information ----------------";
	cout << s1 << endl;
 
	for (auto att_item: attributesStringList)
	{
		for (int i = (6-smartOffset); i < buf_size; i += 12)
		{
			string att_id = att_item.substr(0, 2);
			sprintf(id_arr, "%02X", buf[i]);

			if (att_id.compare(id_arr) == 0)
			{
				int value = 0;

				value = (buf[i + 8] << 24) + (buf[i + 7] << 16) + (buf[i + 6] << 8) + (buf[i + 5]);
				
				cout << att_item << "\t" << value <<"\t"<< endl;
				break;
			}
		}
	}

	::close(fd);
	return 0;
}

int DumpNVMESMARTInfo(string deviceStr)
{
	nvme_Device * nvmeDev;
	nvmeDev = new nvme_Device(deviceStr.c_str(), "", 0);
	int fd = nvmeDev->myOpen();
	if (fd < 0)
		return -1;
	vector<string> SMARTItems = get_SMART_Data_NVMe(nvmeDev, fd);
	string s1 = "---------------- S.M.A.R.T Information ----------------";
	cout << s1 << endl;
	for (int i = 0; i < SMARTItems.size(); i++)
	{
		cout << SMARTItems.at(i) << endl;
	}::close(fd);
	return 0;
}

int DumpSATASCSI_HealthInfo(string deviceStr)
{
	int smartOffset = 0;
	int fd = -1;
	int buf_size = 512;
	fd = ::open(deviceStr.c_str(), O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	char id_arr[3];
	unsigned char buf[buf_size] = { WIN_SMART, 0, SMART_READ_VALUES, 1
	};
	int ret = ioctl(fd, HDIO_DRIVE_CMD, (unsigned char *) buf);
	if(ret != 0)   //SCSI
	{
		smartOffset = 4;
		memset(&buf, 0, 512);
		unsigned char varBuf[512];
		int sg_fd;
		sg_fd = ::open(deviceStr.c_str(), O_RDWR);
		if(SCSI_smart(sg_fd, varBuf, 512)){
			memcpy(&buf, varBuf, sizeof(buf));
		}
		::close(sg_fd);
	}
	
	vector<string> attributesStringList = get_SMART_Attributes(fd);
	string s1 = "---------------- Health Information ----------------";
	cout << s1 << endl;
	string item_spec_erase_count = "A8";
	string item_average_erase_count = "97";
	string item_average_erase_count1 = "A7";
	string item_remain_life = "A9";
	int average_erase_count = -1;
	double spec_erase_count = -1;
	int remain_life = -1;
	for (int i = (6 - smartOffset); i < buf_size; i += 12)
	{
		sprintf(id_arr, "%02X", buf[i]);
		if (item_remain_life.compare(id_arr) == 0)
		{
			remain_life = (buf[i + 8] << 24) + (buf[i + 7] << 16) + (buf[i + 6] << 8) + (buf[i + 5]);
			break;
		}
		if (item_spec_erase_count.compare(id_arr) == 0)
		{
			spec_erase_count = (buf[i + 8] << 24) + (buf[i + 7] << 16) + (buf[i + 6] << 8) + (buf[i + 5]);
		}
		if (item_average_erase_count.compare(id_arr) == 0 || item_average_erase_count1.compare(id_arr) == 0)
		{
			average_erase_count = (buf[i + 8] << 24) + (buf[i + 7] << 16) + (buf[i + 6] << 8) + (buf[i + 5]);
		}
	}
	if(remain_life != -1)
	{
		float healthvalue = remain_life;
		cout << "Health Percentage: " << setprecision(3) << healthvalue << "%" << endl;
	}
	else{
		if (spec_erase_count == -1)
		{
			cout << "Unknown" << endl;
		}
		else
		{
			cout << "Max Erase Count of Spec: " << (int) spec_erase_count << endl;
			cout << "Average Erase Count of Spec: " << average_erase_count << endl;
			float healthvalue = ((spec_erase_count - average_erase_count) / spec_erase_count) *100;
			if(healthvalue<0)
				healthvalue=0;
			cout << "Health Percentage: " << setprecision(3) << healthvalue << "%" << endl;
		}
	}
	::close(fd);
	return 0;
}

int DumpNVMEHealthInfo(string deviceStr)
{
	nvme_Device * nvmeDev;
	nvmeDev = new nvme_Device(deviceStr.c_str(), "", 0);
	int fd = nvmeDev->myOpen();
	if (fd < 0)
		return -1;
	vector<string> SMARTItems = get_SMART_Data_NVMe(nvmeDev, fd);
	string s1 = "------- Health Information -------";
	cout << s1 << endl;
	string item = SMARTItems.at(4);
	string HealthPercentage = item.substr(item.find(":") + 1, strlen(item.c_str()));

	int value = std::atoi(HealthPercentage.c_str());
	if (value > 100)
	{
		value = 0;
	}
	else
	{
		value = 100 - value;
	}
	string s2 = "Health Percentage: " + std::to_string(value) + " %";
	cout << s2 << endl;

	DumpNVMEBadblock(deviceStr);
	DumpNVMEWAI(deviceStr);
	return 0;
}

vector<string> get_SMART_Data_NVMe(nvme_Device *nvmeDev, int fd)
{
	const char *type = "";
	unsigned nsid = 0;	// invalid namespace id -> use default

	/*  nvme_Device * nvmeDev;
	nvmeDev = new nvme_Device(devName, type, nsid);
	int fd = nvmeDev->myOpen(); */

	nvme_Device::nvme_smart_log smart_log;

	nvmeDev->nvme_read_smart_log(smart_log);

	char buf[64];
	//pout("SMART/Health Information (NVMe Log 0x02, NSID 0x%x)\n", nsid);
	vector<string> smart;
	char val[64];

	sprintf(val, "0\tCritical Warning:\t0x%02x", smart_log.critical_warning);
	smart.push_back(std::string(val));
	sprintf(val, "1-2	Composite Temperature:\t%s", to_str(buf, le16_to_uint(smart_log.temperature)));
	smart.push_back(std::string(val));
	sprintf(val, "3	Available Spare:\t%u", smart_log.avail_spare);
	smart.push_back(std::string(val));
	sprintf(val, "4	Available Spare Threshold:\t%u", smart_log.spare_thresh);
	smart.push_back(std::string(val));
	sprintf(val, "5	Percentage Used:\t%u", smart_log.percent_used);
	smart.push_back(std::string(val));
	sprintf(val, "32-47	Data Units Read:\t%s", le128_to_str(buf, smart_log.data_units_read, 1000 *512));
	smart.push_back(std::string(val));
	sprintf(val, "48-63	Data Units Written:\t%s", le128_to_str(buf, smart_log.data_units_written, 1000 *512));
	smart.push_back(std::string(val));
	sprintf(val, "64-79	Host Read Commands:\t%s", le128_to_str(buf, smart_log.host_reads));
	smart.push_back(std::string(val));
	sprintf(val, "80-95	Host Write Commands:\t%s", le128_to_str(buf, smart_log.host_writes));
	smart.push_back(std::string(val));
	sprintf(val, "96-111	Controller Busy Time:\t%s", le128_to_str(buf, smart_log.ctrl_busy_time));
	smart.push_back(std::string(val));
	sprintf(val, "112-127	Power Cycles:\t%s", le128_to_str(buf, smart_log.power_cycles));
	smart.push_back(std::string(val));
	sprintf(val, "128-143	Power On Hours:\t%s", le128_to_str(buf, smart_log.power_on_hours));
	smart.push_back(std::string(val));
	sprintf(val, "144-159	Unsafe Shutdowns:\t%s", le128_to_str(buf, smart_log.unsafe_shutdowns));
	smart.push_back(std::string(val));
	sprintf(val, "160-175	Media and Data integrity Errors:\t%s", le128_to_str(buf, smart_log.media_errors));
	smart.push_back(std::string(val));
	sprintf(val, "176-191	Number of Error information Log Entries:\t%s", le128_to_str(buf, smart_log.num_err_log_entries));
	smart.push_back(std::string(val));
	sprintf(val, "192-195	Warning Composite Temperature Time:\t%d", smart_log.warning_temp_time);
	smart.push_back(std::string(val));
	sprintf(val, "196-199	Critical Composite Temperature Time:\t%d", smart_log.critical_comp_time);
	smart.push_back(std::string(val));::close(fd);

	return smart;
}
int Read_Identity(string deviceStr, unsigned char *buf, uint size)
{
	unsigned char cdb[12];
	unsigned char sense_buf[32];
	//unsigned char *desc = sense_buf + 8;
	sg_io_hdr_t io_hdr;
	int ret = -1;

	memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
	memset(&cdb, 0, 12);
	memset(&sense_buf, 0, 32);
	cdb[0] = 0xA1;
	cdb[1] = 4 << 1;
	cdb[2] = 0x2E;
	cdb[3] = 0x00;
	cdb[4] = 0x01;
	cdb[5] = 0x00;
	cdb[6] = 0x00;
	cdb[7] = 0x00;
	cdb[8] = 0 &0x4F;
	cdb[9] = 0xEC;

	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = sizeof(cdb);
	/*io_hdr.iovec_count = 0; */
	/*memset takes care of this */
	io_hdr.mx_sb_len = sizeof(sense_buf);
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.dxfer_len = size;
	io_hdr.dxferp = buf;
	io_hdr.cmdp = cdb;
	io_hdr.sbp = sense_buf;
	io_hdr.timeout = 120000; /*120000 millisecs == 120 seconds */

	int fd = ::open(deviceStr.c_str(), O_RDONLY | O_NONBLOCK);

	ret = ioctl(fd, SG_IO, &io_hdr);
	if (ret != 0)
	{
		perror("IO Fail");
	}::close(fd);
	return ret;
}
enum SE_Status
{
	Security_Expired = 16,
		Security_Frozen = 8,
		Security_Locked = 4,
		Security_Enabled = 2,
		Security_Unsupported = 1
};

int isSupport(string deviceStr)
{
	int i = 0;
	 string model = "";
	if (deviceStr.find("nvme") != std::string::npos)
	{
		//nvme
		nvme_Device * nvmeDev;
		nvmeDev = new nvme_Device(deviceStr.c_str(), "", 0);
		int fd = nvmeDev->myOpen();
		if (fd < 0)
			return -1;
		char buf[64];
		nvme_Device::nvme_id_ctrl id_ctrl;
		const char *model_str_byte;
		const char *serialno_str_byte;
		const char *
			fwrev_str_byte;
		nvmeDev->nvme_read_id_ctrl(id_ctrl);
 		model_str_byte = format_char_array(buf, id_ctrl.mn);
		model= std::string(model_str_byte);
	}
	else
	{
		struct hd_driveid hd;
		int fd;
		memset(&hd, 0, 512);
		fd = ::open(deviceStr.c_str(), O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			return -1;
		int ret = ioctl(fd, HDIO_GET_IDENTITY, &hd);
		if (ret == 0)  //SATA
		{
			 model =reinterpret_cast<char*> (hd.model);
		}
		else   //SCSI
		{
			memset(&hd, 0, 512);
			unsigned char idTable[512];
			int sg_fd;
			sg_fd = ::open(deviceStr.c_str(), O_RDWR);
			if (sg_fd < 0)
				return false;	
			if(SCSI_id(sg_fd, idTable)){
				memcpy(&hd, idTable, sizeof(hd_driveid));
				model =reinterpret_cast<char*> (hd.model);
				
				string model_rev = "";
				
				int i = 0;
				while(model[i] != '\0')
				{
					model_rev += model[i+1];
					model_rev += model[i];
					i+=2;	
				}
				
				model = model_rev;
			}
		}
		::close(fd);
	}
	 
	if(model.empty())
		return 0;
	else
		return 1;
}

char* grabString(char* data, int start_pos, int length){
	char *str = (char*)malloc( (length + 1) * sizeof(char) );
	strncpy(str, data + start_pos, length);
	str[length] = '\0';
	return str;
}

char* grabHex(char* data, int start_pos, int length){
	char *str = (char*)malloc( (length) * sizeof(char) );
	memcpy(str, data + start_pos, length);
	return str;
}

double hexArrToDec(char *data, int start_pos, int length){
	double tmp = 0;
	for(int i = 0; i < length; i++){
		double shift = (unsigned char)data[start_pos+i] << (4* (2 * ((length-i)-1))); //\A5\AA\B2\BE4byte(16\A6줸)\A1A\A8⭿\B6q(\A8C\A4@\ADӳ椸\A6\B32Byte)
		tmp += shift;
	}
	return tmp;
}

void showGuide()
{
	cout << "Usage:" << endl;
	cout << " " << "SMARTQuery_Cmd" << endl;
	cout << "\tshow all information of devices" << endl;
	cout << " " << "SMARTQuery_Cmd [option] <device>" << endl;
	cout << "\tshow specific information of the device by option" << endl;
	cout << "\nOptions:" << endl;
	cout << " " << "-all:" << "\t\tlist all information of the device" << endl;
	cout << " " << "-id:" << "\t\tlist id table information of the device" << endl;
	cout << " " << "-smart:" << "\tlist S.M.A.R.T table information of the device" << endl;
	cout << " " << "-health:" << "\tlist the health information of the device" << endl;
	cout << " " << "-license:" << "\tdisplay the End User License Agreement and Statement" << endl;
	cout << " " << "-v:" << "\t\tdisplay the application version" << endl;

}
