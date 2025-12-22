from typing import Optional
from abc import ABC, abstractmethod
import sys

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
    arg = arg.strip()
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
        disp = int(arg, 8) & 0o777777
    elif arg[0] in '0123456789-':
        disp = int(arg, 10) & 0o777777
    else:
        label = symbols[arg]
        if mode == 2:
            label -= pc
        disp = label & 0o777777
    
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
            "B": 0,
            "BL": 1,
            "ITN": 2,
            "DTN": 3,
            "TMN": 4,
            "TMZ": 5,
            
            "M": 0x180,
            "MA": 0x181,
            "MNA": 0x182,
            "D": 0x183,
            
            "BSM": 0xE,
            "BRM": 0xF,
            
            "SLR": 0xBF0,
            
            "RFI": 0x1820,
            "RLMSK": 0x1821,
            "LMSK": 0x1822,
            "STMSK": 0x1823,
            "INVSM": 0x1824,
            "INVPG": 0x1825,
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
            "EDIT": 0o001,
            "EDITS": 0o002,
            
            "LX": 0o003,
            "AX": 0o004,
            
            "ITNE": 0o005,
            "DTNE": 0o006,
            
            "LXH": 0o007,
            
            "LCM": 0o010,
            "LN": 0o011,
            "L": 0o012,
            "ST": 0o013,
            
            "AC": 0o014,
            "S": 0o015,
            "A": 0o016,
            "AN": 0o017,
            "O": 0o022,
            "X": 0o026,

            "WAIT": 0o600,
            "INT": 0o601,
            
            "LSK": 0o603,
            "STSK": 0o604,
            
            "LCTL": 0o605,
            "STCTL": 0o606,
            "LXRT": 0o607,
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
        
        "M": (get_paren_number, 7,   7,  0         ),
        "R": (get_paren_number, 7,   0,  0         ),
        "RC": (get_paren_number, 7,   0,  0x4000),
        
        "NL": (static(1),        1,   31, 0         ),
        
        "CC": (static(1),        2,   18, 0         ),
        "SC": (static(2),        2,   18, 0         ),
        "CMC": (static(3),        2,   18, 0         ),
        
        "TNV": (static(1),        3,   15, 0         ),
        "TCN": (static(2),        3,   15, 0         ),
        "TCZ": (static(3),        3,   15, 0         ),
        "TRN": (static(4),        3,   15, 0         ),
        "TRZ": (static(5),        3,   15, 0         ),
        "TCRN": (static(6),        3,   15, 0         ),
        "TCRZ": (static(7),        3,   15, 0         ),
    }
    
    argspl = arg.split("(")[0]
    fn, bits, shl, extra = args_tbl[argspl.upper()]
    return ((fn(arg.strip()[len(argspl):]) & ((1 << bits) - 1)) << shl) | extra

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
            "CMA": 0xE0,
            "NA": 0xE1,
            "LA": 0xE2,
            "IA": 0xE3,
            "ACA": 0xE4,
            "SA": 0xE5,
            "AA": 0xE6,
            "ANA": 0xE7,
            "OA": 0xF2,
            "XA": 0xF6,
        }

    def will_assemble(self, card: Card) -> bool:
        return (card.command.strip().upper()[0:3] in self.opcodes)

class AssembleBX(AssemblerModule):
    def size(self, card: Card) -> int:
        return 1
    
    def assemble(
        self,
        card: Card,
        symbols: dict[str, int],
        pc: int
    ) -> list[int]:
        args = card.argument.strip().split(",")
        if len(args) > 1 and len(args) < 4:
            src = int(args[0])
            tgt = int(args[1])
            if len(args) == 3:
                bs = int(args[2])
                if bs < 0 or bs > 63:
                    raise ValueError("Bad byte size")
            else:
                bs = 0

            opcode = self.opcodes[card.command.strip().upper()]
            return [(opcode << 27) | (src << 23) | (tgt << 18) | bs]
        else:
            raise ValueError("Syntax error")

        
    def __init__(self):
        self.opcodes = {
            "LC": 0o040,
            "STC": 0o041,
            "ICX": 0o042,
            "ILC": 0o043,
            "ISTC": 0o044
        }

class AssembleHelper0(AssemblerModule):
    def size(self, card: Card) -> int:
        return 1
    
    def assemble(
        self,
        card: Card,
        symbols: dict[str, int],
        pc: int
    ) -> list[int]:
        if card.argument is None:
            opcode = self.opcodes[card.command.strip().upper()]
            return [opcode]
        else:
            raise ValueError("Syntax error")

        
    def __init__(self):
        self.opcodes = {
            "HLT": 0o600002000001,
            "NOP": 0o700010040000,
            "CC": 0o700011040000,
            "SC": 0o700012040000,
            "CMC": 0o700013040000,
            "TNV": 0o700010140000,
            "TCN": 0o700010240000,
            "TCZ": 0o700010340000,
            "TACN": 0o700010440000,
            "TACZ": 0o700010540000,
            "TCACN": 0o700010640000,
            "TCACZ": 0o700010740000
        }

def ascii7(string: str) -> list[int]:
    result = []
    char = 0
    shamt = 29

    i = 0
    while i < len(string):
        if string[i] == "\\":
            i += 1
            octal = string[i:i+3]
            i += 3
            c = int(octal, 8)
        else:
            c = ord(string[i])
            i += 1
            
        char |= (c & 0x7F) << shamt
        shamt -= 7
        if shamt < 1:
            shamt = 29
            result.append(char)
            char = 0
    result.append(char)
    return result

class AssembleData(AssemblerModule):
    def size(self, card: Card) -> int:
        if card.command == "ASCII":
            return len(ascii7(card.argument.rstrip()))
        elif card.command == "USING":
            return 1
        else:
            args = card.argument.strip().split(",")
            return len(args)
    
    def assemble(
        self,
        card: Card,
        symbols: dict[str, int],
        pc: int
    ) -> list[int]:
        command = card.command.strip().upper()
        args = card.argument.strip().split(",")
        if command == "DW":
            result = []
            for arg in args:
                if arg[0] == '0' or arg[0:2] == '-0':
                    result.append(int(arg, 8) & 0o777777777777)
                elif arg[0] in '0123456789-':
                    result.append(int(arg, 10) & 0o777777777777)
                else:
                    result.append(symbols[arg])
            return result
        elif command == "USING":
            result = 0
            for arg in args:
                if len(arg.split("-")) == 2:
                    start_end = arg.split("-")
                    start = int(start_end[0])
                    end = int(start_end[1])
                    
                    if (
                        (start < 0 or start > 15) or
                        (end < 0 or end > 15) or
                        (start >= end)
                    ):
                        raise ValueError("Bad USING range")
                    
                    for i in range(start, end + 1):
                        result |= 1 << (15 - i)
                elif "-" not in arg:
                    i = int(arg)
                    if i < 0 or i > 15:
                        raise ValueError("Bad USING register")
                    else:
                        result |= 1 << (15 - i)
                else:
                    raise ValueError("Bad USING register")
            return [result]
        elif command == "ASCII":
            return ascii7(card.argument.rstrip())
        else:
            raise ValueError("Syntax error")
        
    def __init__(self):
        self.opcodes = {
            "DW": 0,
            "USING": 0,
            "ASCII": 0
        }

class AssembleIO(AssemblerModule):
    def size(self, card: Card) -> int:
        return 1
    
    def assemble(
        self,
        card: Card,
        symbols: dict[str, int],
        pc: int
    ) -> list[int]:
        args = card.argument.strip().split(",")
        command = card.command.strip().upper()
        opcode = self.opcodes[command]
        dev = args[0]

        if dev[0] == '0' or dev[0:2] == '-0':
           result = int(dev, 8) & 0o7777
        elif dev[0] in '0123456789-':
            result = int(dev, 10) & 0o7777
        
        result |= (0o670 << 27) | (opcode << 12)
        
        if command[0:2] == "WI" or command[0:2] == "RI":
            register = int(args[1])
            if register < 0 or register > 15:
                raise ValueError("No such register")
            
            buffer = int(args[2])
            if buffer < 0 or buffer > 6:
                raise ValueError("No such buffer")
            
            result |= (register << 23) | (buffer << 13)
        
        return [result]
        
    def __init__(self):
        self.opcodes = {
            "NIO": 0x0F,
            "NIOS": 0x1F,
            "NIOC": 0x2F,
            "NIOP": 0x3F,
            "RIO": 0x00,
            "RIOS": 0x10,
            "RIOC": 0x20,
            "RIOP": 0x30,
            "WIO": 0x01,
            "WIOS": 0x11,
            "WIOC": 0x21,
            "WIOP": 0x31,
            "TIONB": 0x0E,
            "TIOBZ": 0x1E,
            "TIOND": 0x2E,
            "TIODN": 0x3E,
        }

class AssembleCommand(AssemblerModule):
    def size(self, card: Card) -> int:
        return 0
    
    def assemble(
        self,
        card: Card,
        symbols: dict[str, int],
        pc: int
    ) -> list[int]:
        args = card.argument.strip().split(",")
        result = []
        if len(args) == 1:
            arg = args[0]
            if arg[0] == '0' or arg[0:2] == '-0':
                disp = int(arg, 8)
            elif arg[0] in '0123456789-':
                disp = int(arg, 10)
            
            command = card.command.strip().upper()
            if command == "BSS":
                result.append((pc + disp) & 0o777777777)
            elif command == "ORIGIN":
                result.append(disp & 0o777777777)
            else:
                raise ValueError("Syntax error")
                
            return result
        else:
            raise ValueError("Syntax error")
        
    def __init__(self):
        self.opcodes = {
            "BSS": 0,
            "ORIGIN": 0
        }

helpers = {
    "TRN": ("LA", "{},0,NL,TRN"),
    "TRZ": ("LA", "{},0,NL,TRZ"),
    "TCRN": ("LA", "{},0,NL,TCRN"),
    "TCRZ": ("LA", "{},0,NL,TCRZ"),
    "MSAC": ("LA", "0,0,M({})"),
    "CMAC": ("LA", "0,0,CC,M({})"),
    "SMAC": ("LA", "0,0,SC,M({})"),
    "RAC": ("LA", "0,0,R({})"),
    "RCAC": ("LA", "0,0,RC({})"),
    "TACBZ": ("LA", "0,0,CC,M(35),R({}),NL,TRZ"),
    "TACBN": ("LA", "0,0,CC,M(35),R({}),NL,TRN"),
    "TRNE": ("SA", "{},{},NL,TRN"),
    "TREQ": ("SA", "{},{},NL,TRZ"),
    "TRGT": ("SA", "{},{},SC,NL,TCRN"),
    "TRLE": ("SA", "{},{},SC,NL,TCRZ"),
    "TRLT": ("SA", "{},{},CC,NL,TCRN"),
    "TRGE": ("SA", "{},{},CC,NL,TCRZ"),
    "MSK": ("LA", "{0},{0},M({1})"),
    "CMK": ("LA", "{0},{0},CC,M({1})"),
    "SMK": ("LA", "{0},{0},SC,M({1})"),
    "R": ("LA", "{0},{0},R({1})"),
    "RC": ("LA", "{0},{0},RC({1})"),
    "BRM": ("BRM", "0"),
    "SLR": ("SLR", "0")
}

class Assembler:
    symbols: dict[str, int]
    pc: int
    cards: list[Card]
    
    output: dict[int, list[int]]
    
    modules: list[AssemblerModule]
    
    def __init__(self, filename: str):
        self.symbols = {}
        self.pc = 0
        self.cards = []
        self.output = {}
        
        self.modules = [
            AssembleMR(),
            AssembleAM(),
            AssembleAA(),
            AssembleBX(),
            AssembleHelper0(),
            AssembleIO(),
            AssembleData()
        ]
        
        self.commands = AssembleCommand()
        
        with open(filename) as file:
            for line in file:
                if line.rstrip() != "":
                    card = Card(line.rstrip())
                    
                    command = (
                        card.command.strip().upper() if card.command else None
                    )
                    if command in helpers:
                        args = card.argument.strip().split(",") if card.argument else ""
                        card.command = helpers[command][0]
                        card.argument = helpers[command][1].format(*args)
                    
                    self.cards.append(card)
    
    def get_syms(self):
        for card in self.cards:
            if card.symbol is not None:
                self.symbols[card.symbol] = self.pc
        
            if card.command is not None:
                is_command = True
                for module in self.modules:
                    if module.will_assemble(card):
                        is_command = False
                        self.pc += module.size(card)
                if is_command:
                    self.pc = (
                        self.commands.assemble(card, self.symbols, self.pc)[0]
                    )
    
    def assemble(self):
        current_sym = 0
        for card in self.cards:        
            if card.command is not None:
                is_command = True
                for module in self.modules:
                    if module.will_assemble(card):
                        self.output[current_sym] = (
                            self.output.get(current_sym, []) + (
                                module.assemble(card, self.symbols, self.pc)
                            )
                        )
                        is_command = False
                        self.pc += module.size(card)
                if is_command:
                    old_pc = self.pc
                    self.pc = (
                        self.commands.assemble(card, self.symbols, self.pc)[0]
                    )
                    if old_pc != self.pc:
                        current_sym = self.pc

    def print_ppt(self):
        keys = list(self.output.keys())
        for k in range(0, len(keys)):
            blk = keys[k]
            print(f"{blk:0{9}o}", end = "")
            for i in self.output[blk]:
                for sh in range(30, -1, -6):
                    c = ((i >> sh) & 0o77) + 32
                    print(chr(c), end = "")
            if k == len(keys) - 1:
                print("~")
            else:
                print("|", end = "")
    
    def print_c(self):
        keys = list(self.output.keys())
        for k in range(0, len(keys)):
            blk = keys[k]
            pc = blk
            for i in self.output[blk]:
                print(f"    cpu.memory[{pc}] = 0{i:0{12}o};")
                pc += 1

if __name__ == "__main__":
    assembler = Assembler(sys.argv[1])
    assembler.get_syms()
    assembler.assemble()
    if "-c" in sys.argv:
        assembler.print_c()
    else:
        assembler.print_ppt()
    
