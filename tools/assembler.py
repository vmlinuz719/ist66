from typing import Optional
from abc import ABC, abstractmethod

class Card:
    symbol: Optional[str]
    command: Optional[str]
    argument: Optional[str]
    comment: Optional[str]
    number: Optional[str]
    
    def __init__(self, text: str):
        full_card = '{:<80}'.format(text[:80])
        
        self.symbol = full_card[0:8].rstrip() or None
        
        if full_card[8] == '*':
            self.comment = full_card[9:72].rstrip() or None
            self.command = None
            self.argument = None
        
        else:
            self.comment = full_card[48:72].rstrip() or None
            self.command = full_card[9:17].rstrip() or None
            self.argument = full_card[17:48].rstrip() or None
        
        self.number = full_card[72:80].rstrip()

def generate_address(
    arg: str,
    symbols: Optional[dict[str, int]] = None,
    pc: int = 0
) -> int:
    """
    123456          displacement only
    _123456         use direct page
    .123456         PC relative
    123456(4        X4 relative, closing parenthesis optional
    123456+         A13 post-increment
    123456-         A13 pre-decrement
    @               indirect
    Leading zero for octal, leading any other digit for decimal
    Leading letter for label
    """
    if arg[0] == '@':
        indirect = True
        arg = arg[1:]
    else:
        indirect = False
    
    if arg[0] == '_':
        mode = 1 # direct page
        arg = arg[1:]
    elif arg[0] == '.':
        mode = 2 # relative
        arg = arg[1:]
    elif len(arg.split('(')) == 2:
        args = arg.split('(') # indexed
        mode = int(args[1].rstrip(')'))
        if mode > 13 or mode < 3:
            raise ValueError("No such index register")
        arg = args[0]
    elif arg[-1] == '+':
        mode = 14 # A13++
        arg = arg[:-1]
    elif arg[-1] == '-':
        mode = 15 # --A13
        arg = arg[:-1]
    else:
        mode = 0 # absolute
    
    if arg[0] == '0' or arg[0:2] == '-0':
        disp = int(arg, 8) & 0o777777777
    elif arg[0] in '0123456789-':
        disp = int(arg, 10) & 0o777777777
    else:
        label = symbols[arg]
        if mode == 2:
            label -= pc
        disp = label & 0o777777777
    
    result = (mode << 18) | disp
    if indirect:
        result |= 1 << 22
    return result

class AssemblerModule(ABC):
    @abstractmethod
    def size(self, card: Card) -> int:
        pass
        
    @abstractmethod
    def assemble(
        self,
        card: Card,
        symbols: dict[str, int],
        pc: int
    ) -> list[int]:
        pass
    
    opcodes: dict[str, int]
    
    @abstractmethod
    def __init__(self):
        self.opcodes = {}
    
    def will_assemble(self, card: Card) -> bool:
        return (card.command.strip().upper() in self.opcodes)

class AssembleMR(AssemblerModule):
    def size(self, card: Card) -> int:
        return 1
    
    def assemble(
        self,
        card: Card,
        symbols: dict[str, int],
        pc: int
    ) -> list[int]:
        address = generate_address(card.argument.strip(), symbols, pc)
        return [(self.opcodes[card.command.strip().upper()] << 23) | address]
    
    def __init__(self):
        self.opcodes = {
            "JMP": 0,
            "JSR": 1,
            "ISZ": 2,
            "DSZ": 3,
            
            "MPY": 0x180,
            "MPA": 0x181,
            "MNA": 0x182,
            "DIV": 0x183,
            
            "CLM": 0x400,
            "RTM": 0x401,
            
            "XIN": 0x1820,
            "RMS": 0x1821,
            "LMS": 0x1822,
            "DMS": 0x1823,
        }

class AssembleAM(AssemblerModule):
    def size(self, card: Card) -> int:
        return 1
    
    def assemble(
        self,
        card: Card,
        symbols: dict[str, int],
        pc: int
    ) -> list[int]:
        args = card.argument.strip().split(",")
        if len(args) == 2:
            register = int(args[0])
            if register < 0 or register > 15:
                raise ValueError("No such register")
        elif len(args) == 1:
            register = 0
        else:
            raise ValueError("Syntax error")
            
        address = generate_address(args[-1], symbols, pc)
        return [(
            (self.opcodes[card.command.strip().upper()] << 27)
            | (register << 23)
            | address
        )]
    
    def __init__(self):
        self.opcodes = {
            "EDT": 0o001,
            "ESK": 0o002,
            
            "LAD": 0o003,
            "AAD": 0o004,
            
            "ISE": 0o005,
            "DSE": 0o006,
            
            "LAS": 0o007,
            
            "LCO": 0o010,
            "LNG": 0o011,
            "LAC": 0o012,
            "DAC": 0o013,
            
            "ADC": 0o014,
            "SUB": 0o015,
            "ADD": 0o016,
            "AND": 0o017,
            "IOR": 0o022,
            "XOR": 0o026,
            
            "HLT": 0o600,
            "INT": 0o601,
            
            "LMK": 0o603,
            "DMK": 0o604,
            
            "LCT": 0o605,
            "DCT": 0o606,
        }

def get_paren_arg(arg: str) -> str:
    spl = arg.strip().split('(')
    return spl[1].rstrip(')').strip()

def get_paren_number(arg: str) -> int:
    contents = get_paren_arg(arg)
    if contents[0] == '0' or contents[0:2] == '-0':
        return int(contents, 8) & 0o777777777
    elif contents[0] in '0123456789-':
        return int(contents, 10) & 0o777777777

def assemble_aa_arg(arg: str) -> int:
    def static(x):
        return lambda y: x
        
    args_tbl = {
        #       fn                bits shl extra
        "SDA": (get_paren_number, 4,   7,  0x2000    ),
        
        "MSK": (get_paren_number, 7,   7,  0         ),
        "RTA": (get_paren_number, 7,   0,  0         ),
        "RTC": (get_paren_number, 7,   0,  0x80000000),
        
        "NLA": (static(1),        1,   14, 0         ),
        
        "CLC": (static(1),        2,   18, 0         ),
        "STC": (static(2),        2,   18, 0         ),
        "CMC": (static(3),        2,   18, 0         ),
        
        "SKP": (static(1),        3,   15, 0         ),
        "SZC": (static(2),        3,   15, 0         ),
        "SNC": (static(3),        3,   15, 0         ),
        "SZR": (static(4),        3,   15, 0         ),
        "SNR": (static(5),        3,   15, 0         ),
        "SEZ": (static(6),        3,   15, 0         ),
        "SBN": (static(7),        3,   15, 0         ),
    }
    
    fn, bits, shl, extra = args_tbl[arg[0:3]]
    return ((fn(arg[3:]) & ((1 << bits) - 1)) << shl) | extra

class AssembleAA(AssemblerModule):
    def size(self, card: Card) -> int:
        return 1
    
    def assemble(
        self,
        card: Card,
        symbols: dict[str, int],
        pc: int
    ) -> list[int]:
        args = card.argument.strip().split(",")
        if len(args) >= 2:
            src = int(args[0])
            tgt = int(args[1])
            if src < 0 or src > 15 or tgt < 0 or tgt > 15:
                raise ValueError("No such register")
            result = (src << 27) | (tgt << 23)
            for arg in args[2:]:
                result |= assemble_aa_arg(arg)
            opcode = self.opcodes[card.command.strip().upper()]
            opcode_split = ((opcode >> 4) << 32) | ((opcode & 7) << 20)
            return [result | opcode_split]
        else:
            raise ValueError("Syntax error")

        
    def __init__(self):
        self.opcodes = {
            "OCA": 0xE0,
            "NEA": 0xE1,
            "DAA": 0xE2,
            "ICA": 0xE3,
            "ACA": 0xE4,
            "SUA": 0xE5,
            "ADA": 0xE6,
            "ANA": 0xE7,
            "IOA": 0xF2,
            "XOA": 0xF6,
        }

    def will_assemble(self, card: Card) -> bool:
        return (card.command.strip().upper()[0:3] in self.opcodes)

class Assembler:
    symbols: dict[str, int]
    
    

"""
        * A longer comment blah blah blah                               00001000
LABEL008 ADA     1,2,CLC,SDA(3),RTA(3),SZR      This is the comment     00001020

>>> string[0:8].strip()
'LABEL008'
>>> string[8].strip()
''
>>> string[9:15].strip()
'ADD'
>>> string[15:48].strip()
"0,7,'123456701"
>>> string[48:72].strip()
'This is the comment'
>>> string[72:80].strip()
'00001020'
"""