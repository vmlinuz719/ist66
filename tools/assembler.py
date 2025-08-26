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
            self.command = full_card[9:15].rstrip() or None
            self.argument = full_card[15:48].rstrip() or None
        
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
    ) -> int:
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
    ) -> int:
        address = generate_address(card.argument.strip(), symbols, pc)
        return (self.opcodes[card.command.strip().upper()] << 23) | address
    
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
    ) -> int:
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
        return (
            (self.opcodes[card.command.strip().upper()] << 27)
            | (register << 23)
            | address
        )
    
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

class Assembler:
    symbols: dict[str, int]
    
    

"""
        * A longer comment blah blah blah                               00001000
LABEL008 DIV   123(6)                           This is the comment     00001020

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