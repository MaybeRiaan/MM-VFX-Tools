#include "GrainTransfer.h"

typedef struct {
	A_u_long	index;
	A_char		str[256];
} TableString;

TableString		g_strs[StrID_NUMTYPES] = {
	StrID_NONE,			"",
	StrID_Name,			"Grain Transfer",
	StrID_Description,	"Transfers the measured grain of a source plate onto this layer: analyze once, phase-randomized regeneration, ghost-free and seam-free, cheap per frame.",
};

char	*GetStringPtr(int strNum)
{
	return g_strs[strNum].str;
}
