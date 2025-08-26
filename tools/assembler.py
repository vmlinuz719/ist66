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
    
    print(arg)
    
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
    def size(self, arg: str) -> int:
        pass
    
    def assemble(self, arg: str) -> int:
        pass

class Assembler:
    symbols: dict[str, int]
    
    

"""
        * A longer comment blah blah blah                               00001000
LABEL008 ADD   0,7,'123456701                   This is the comment     00001020

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